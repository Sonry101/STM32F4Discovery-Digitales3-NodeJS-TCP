
#include "main.h"
#include "stm32_ub_led.h"
#include "stm32_ub_adc1_single.h"


static struct tcp_pcb *tcp_echoserver_pcb;

enum tcp_echoserver_states
{
  ES_NONE = 0,
  ES_ACCEPTED,
  ES_RECEIVED,
  ES_CLOSING
};

struct tcp_echoserver_struct
{
  u8_t state;             /* current connection state */
  struct tcp_pcb *pcb;    /* pointer on the current tcp_pcb */
  struct pbuf *p;         /* pointer on the received/to be transmitted pbuf */
};

uint32_t LocalTime = 0;
char temperatura[6];

#define TempConv 10

static err_t tcp_echoserver_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_echoserver_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void tcp_echoserver_error(void *arg, err_t err);
static err_t tcp_echoserver_poll(void *arg, struct tcp_pcb *tpcb);
static err_t tcp_echoserver_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static void tcp_echoserver_send(struct tcp_pcb *tpcb, struct tcp_echoserver_struct *es);
static void tcp_echoserver_connection_close(struct tcp_pcb *tpcb, struct tcp_echoserver_struct *es);
void tcp_echoserver_init(void);
void respuesta(struct tcp_pcb *tpcb, struct tcp_echoserver_struct *es);
void getTemperature();


int main(void)
{

/* - Inicializacion del Sistema - */
	SystemInit();
	UB_Led_Init();
	ETH_BSP_Config();				//Inicializacion de Modulo DP83848
	LwIP_Init();					//Inicializacion de Red
	tcp_echoserver_init();			//Inicializacion de Servidor TCP
	UB_ADC1_SINGLE_Init();			//Inicializacion de ADC
/* -------------------------------*/

	UB_Led_On(LED_GREEN); 			//Inicializacion CORRECTA

	while(1){

	    /* check if any packet received */
	    if (ETH_CheckFrameReceived()) {
	    	/* process received ethernet packet */
	    	UB_Led_Toggle(LED_BLUE);
	    	LwIP_Pkt_Handle();
	    }
	    /* handle periodic timers for LwIP */
	    LwIP_Periodic_Handle(LocalTime);
  }
}

void tcp_echoserver_init(void)
{
  /* create new tcp pcb */
  tcp_echoserver_pcb = tcp_new();

  if (tcp_echoserver_pcb != NULL) {
    err_t err;

    /* bind echo_pcb to port 7 (ECHO protocol) */
    err = tcp_bind(tcp_echoserver_pcb, IP_ADDR_ANY, 7);

    if (err == ERR_OK) {
      /* start tcp listening for echo_pcb */
      tcp_echoserver_pcb = tcp_listen(tcp_echoserver_pcb);

      /* initialize LwIP tcp_accept callback function */
      tcp_accept(tcp_echoserver_pcb, tcp_echoserver_accept);
    } else {
      //printf("Can not bind pcb\n");
    }
  } else {
    //printf("Can not create new pcb\n");
  }
}

/**
  * @brief  This function is the implementation of tcp_accept LwIP callback
  * @param  arg: not used
  * @param  newpcb: pointer on tcp_pcb struct for the newly created tcp connection
  * @param  err: not used
  * @retval err_t: error status
  */
static err_t tcp_echoserver_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  err_t ret_err;
  struct tcp_echoserver_struct *es;

  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(err);

  /* set priority for the newly accepted tcp connection newpcb */
  tcp_setprio(newpcb, TCP_PRIO_NORMAL);

  /* allocate structure es to maintain tcp connection informations */
  es = (struct tcp_echoserver_struct *)mem_malloc(sizeof(struct tcp_echoserver_struct));
  if (es != NULL) {
    es->state = ES_ACCEPTED;
    es->pcb = newpcb;
    es->p = NULL;

    /* pass newly allocated es structure as argument to newpcb */
    tcp_arg(newpcb, es);

    /* initialize lwip tcp_recv callback function for newpcb  */
    tcp_recv(newpcb, tcp_echoserver_recv);

    /* initialize lwip tcp_err callback function for newpcb  */
    tcp_err(newpcb, tcp_echoserver_error);

    /* initialize lwip tcp_poll callback function for newpcb */
    tcp_poll(newpcb, tcp_echoserver_poll, 1);

    ret_err = ERR_OK;
  } else {
    /* return memory error */
    ret_err = ERR_MEM;
  }
  return ret_err;
}


/**
  * @brief  This function is the implementation for tcp_recv LwIP callback
  * @param  arg: pointer on a argument for the tcp_pcb connection
  * @param  tpcb: pointer on the tcp_pcb connection
  * @param  pbuf: pointer on the received pbuf
  * @param  err: error information regarding the reveived pbuf
  * @retval err_t: error code
  */
static err_t tcp_echoserver_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
  struct tcp_echoserver_struct *es;
  err_t ret_err;

  LWIP_ASSERT("arg != NULL",arg != NULL);

  es = (struct tcp_echoserver_struct *)arg;

  /* if we receive an empty tcp frame from client => close connection */
  if (p == NULL) {
    /* remote host closed connection */
    es->state = ES_CLOSING;
    if (es->p == NULL) {
       /* we're done sending, close connection */
       tcp_echoserver_connection_close(tpcb, es);
    } else {
      /* we're not done yet */
      /* acknowledge received packet */
      tcp_sent(tpcb, tcp_echoserver_sent);

      /* send remaining data*/
      tcp_echoserver_send(tpcb, es);
    }
    ret_err = ERR_OK;
  }
  /* else : a non empty frame was received from client but for some reason err != ERR_OK */
  else if(err != ERR_OK) {
    /* free received pbuf*/
    if (p != NULL) {
      es->p = NULL;
      pbuf_free(p);
    }
    ret_err = err;
  } else if(es->state == ES_ACCEPTED) {
    /* first data chunk in p->payload */
    es->state = ES_RECEIVED;

    /* store reference to incoming pbuf (chain) */
    es->p = p;

    /* initialize LwIP tcp_sent callback function */
    tcp_sent(tpcb, tcp_echoserver_sent);

    /* read information and send data */
    respuesta(tpcb, es);


    ret_err = ERR_OK;
  } else if (es->state == ES_RECEIVED) {
    /* more data received from client and previous data has been already sent*/ //Posible Fuente de ERROR
    if (es->p == NULL) {
      es->p = p;

      /* read information and send data */
      respuesta(tpcb, es);

    } else {
      struct pbuf *ptr;

      /* chain pbufs to the end of what we recv'ed previously  */
      ptr = es->p;
      pbuf_chain(ptr,p);
    }
    ret_err = ERR_OK;
  }

  /* data received when connection already closed */
  else {
    /* Acknowledge data reception */
    tcp_recved(tpcb, p->tot_len);

    /* free pbuf and do nothing */
    es->p = NULL;
    pbuf_free(p);
    ret_err = ERR_OK;
  }
  return ret_err;
}

/**
  * @brief  This function implements the tcp_err callback function (called
  *         when a fatal tcp_connection error occurs.
  * @param  arg: pointer on argument parameter
  * @param  err: not used
  * @retval None
  */
static void tcp_echoserver_error(void *arg, err_t err)
{
  struct tcp_echoserver_struct *es;

  LWIP_UNUSED_ARG(err);

  es = (struct tcp_echoserver_struct *)arg;
  if (es != NULL) {
    /*  free es structure */
    mem_free(es);
  }
}

/**
  * @brief  This function implements the tcp_poll LwIP callback function
  * @param  arg: pointer on argument passed to callback
  * @param  tpcb: pointer on the tcp_pcb for the current tcp connection
  * @retval err_t: error code
  */
static err_t tcp_echoserver_poll(void *arg, struct tcp_pcb *tpcb)
{
  err_t ret_err;
  struct tcp_echoserver_struct *es;

  es = (struct tcp_echoserver_struct *)arg;
  if (es != NULL) {
    if (es->p != NULL) {
      /* there is a remaining pbuf (chain) , try to send data */
      tcp_echoserver_send(tpcb, es);
    } else {
      /* no remaining pbuf (chain)  */
      if (es->state == ES_CLOSING) {
        /*  close tcp connection */
        tcp_echoserver_connection_close(tpcb, es);
      }
    }
    ret_err = ERR_OK;
  } else {
    /* nothing to be done */
    tcp_abort(tpcb);
    ret_err = ERR_ABRT;
  }
  return ret_err;
}

/**
  * @brief  This function implements the tcp_sent LwIP callback (called when ACK
  *         is received from remote host for sent data)
  * @param  None
  * @retval None
  */
static err_t tcp_echoserver_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
  struct tcp_echoserver_struct *es;

  LWIP_UNUSED_ARG(len);

  es = (struct tcp_echoserver_struct *)arg;

  if (es->p != NULL) {
    /* still got pbufs to send */
    tcp_echoserver_send(tpcb, es);
  } else {
    /* if no more data to send and client closed connection*/
    if (es->state == ES_CLOSING) {
      tcp_echoserver_connection_close(tpcb, es);
    }
  }
  return ERR_OK;
}


/**
  * @brief  This function is used to send data for tcp connection
  * @param  tpcb: pointer on the tcp_pcb connection
  * @param  es: pointer on echo_state structure
  * @retval None
  */
static void tcp_echoserver_send(struct tcp_pcb *tpcb, struct tcp_echoserver_struct *es)
{
  struct pbuf *ptr;
  err_t wr_err = ERR_OK;

  while ((wr_err == ERR_OK) &&
         (es->p != NULL) &&
         (es->p->len <= tcp_sndbuf(tpcb)))
  {

    /* get pointer on pbuf from es structure */
    ptr = es->p;

    /* enqueue data for transmission */
    wr_err = tcp_write(tpcb, temperatura, strlen((char*)temperatura), 1);

    if (wr_err == ERR_OK) {
      u16_t plen;

      plen = strlen((char*)temperatura);

      /* continue with next pbuf in chain (if any) */
      es->p = ptr->next;

      if (es->p != NULL) {
        /* increment reference count for es->p */
        pbuf_ref(es->p);
      }

      /* free pbuf: will free pbufs up to es->p (because es->p has a reference count > 0) */
      pbuf_free(ptr);

      /* Update tcp window size to be advertized : should be called when received
      data (with the amount plen) has been processed by the application layer */
      tcp_recved(tpcb, plen);
   } else if(wr_err == ERR_MEM) {
      /* we are low on memory, try later / harder, defer to poll */
     es->p = ptr;
   } else {
     /* other problem ?? */
   }
  }
}

/**
  * @brief  This functions closes the tcp connection
  * @param  tcp_pcb: pointer on the tcp connection
  * @param  es: pointer on echo_state structure
  * @retval None
  */
static void tcp_echoserver_connection_close(struct tcp_pcb *tpcb, struct tcp_echoserver_struct *es)
{

  /* remove all callbacks */
  tcp_arg(tpcb, NULL);
  tcp_sent(tpcb, NULL);
  tcp_recv(tpcb, NULL);
  tcp_err(tpcb, NULL);
  tcp_poll(tpcb, NULL, 0);

  /* delete es structure */
  if (es != NULL) {
    mem_free(es);
  }

  /* close tcp connection */
  tcp_close(tpcb);
}

void SysTick_Handler(void)
{
  // incrementiert den Counter

  LocalTime += 10;  // +10 weil Timerintervall = 10ms
}

void respuesta(struct tcp_pcb *tpcb, struct tcp_echoserver_struct *es){

	/* es->p->payload => Mensaje recibido */

	if(strcmp(es->p->payload, "ON")==0){
		UB_Led_On(LED_ORANGE);				//Turn ON Flag
		strcpy(temperatura, "OK");			//Respuesta al servidor
	}
	else if(strcmp(es->p->payload, "OFF")==0){
		UB_Led_Off(LED_ORANGE);				//Turn OFF Flag
		strcpy(temperatura, "OK");			//Respuesta al servidor
	}
	else if(strcmp(es->p->payload, "TEM")==0){
		getTemperature();					//Obtener y enviar Temperatura
	}
	else
		strcpy(temperatura, "ERROR");		//Mesaje incorrecto

	tcp_echoserver_send(tpcb, es);			//Envio de respuesta

	return;
}

void getTemperature(){

	uint16_t adc_lectura;
	char abc[6] = "T";

	adc_lectura = UB_ADC1_SINGLE_Read_MW(ADC_PA3);	//Lectura del valor del ADC

	itoa(adc_lectura, temperatura, 10);

	strcat(abc, temperatura);				//Formato de envio de informacion
	strcpy(temperatura, abc);				// "T123"

	return;
}
