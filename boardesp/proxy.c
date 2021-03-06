#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "user_interface.h"
#include "espconn.h"

#include "tcp_ota.h"
#include "driver/spi_interface.h"

static const int pin = 2;
static volatile os_timer_t some_timer;

// Structure holding the TCP connection information.
struct espconn tcp_conn;
// TCP specific protocol structure.
esp_tcp tcp_proto;

uint8_t buf[0x40*0x10];

void ICACHE_FLASH_ATTR some_timerfunc(void *arg)
{
  // uart testing
  /*uart0_sendStr("hello uart0\n");
  uart1_sendStr_no_wait("hello uart1\n");*/
  //uart0_sendStr("hello uart0\n");

  if (GPIO_REG_READ(GPIO_OUT_ADDRESS) & (1 << pin)) {
    // set gpio low
    gpio_output_set(0, (1 << pin), 0, 0);
  } else {
    // set gpio high
    gpio_output_set((1 << pin), 0, 0, 0);
  }

  // *** SPI MODE ***

  uint32_t value = 0xD3D4D5D6;
  uint32_t sendData[8] = {0};
  SpiData spiData;

  int i = 0;
  int first = 1;
  while (i < 0x40) {
    spiData.cmd = 2;
    spiData.cmdLen = 0;
    spiData.addr = NULL;
    spiData.addrLen = 0;
    spiData.data = sendData;
    spiData.dataLen = 16;

    // manual CS pin
    gpio_output_set(0, (1 << 5), 0, 0);
    SPIMasterRecvData(SpiNum_HSPI, &spiData);
    gpio_output_set((1 << 5), 0, 0, 0);

    if (sendData[0] != 0) {
      memcpy(buf + i*0x10, sendData, 0x10);
      i++;
    } else {
      if (first == 0) break;
    }
    first = 0;
  }

  if (i != 0) {
    espconn_send(&tcp_conn, buf, i*0x10);
  }
}

int did_start_timer = 0;

void ICACHE_FLASH_ATTR tcp_connect_cb(void *arg) {
  struct espconn *conn = (struct espconn *)arg;
  espconn_set_opt(&tcp_conn, ESPCONN_NODELAY);

  /*char message[] = "hello\r\n";
  uint16_t len = strlen(message);
  espconn_send (&tcp_conn, message, len);*/

  if (!did_start_timer) {
    // not atomic!
    did_start_timer = 1;
    // setup timer (100ms, repeating)
    os_timer_setfn(&some_timer, (os_timer_func_t *)some_timerfunc, NULL);
    os_timer_arm(&some_timer, 50, 1);
  }
}

void ICACHE_FLASH_ATTR wifi_init() {
  // start wifi AP
  wifi_set_opmode(SOFTAP_MODE);
  struct softap_config config;
  wifi_softap_get_config(&config);
  char ssid[32];
  os_sprintf(ssid, "panda-%08x", system_get_chip_id()); 
  char password[] = "testing123"; //password must be 8 characters or longer
  strcpy(config.ssid, ssid); 
  strcpy(config.password, password);
  config.ssid_len = strlen(ssid);
  config.authmode = AUTH_WPA2_PSK;
  config.beacon_interval = 100;
  config.max_connection = 10;
  wifi_softap_set_config(&config);

  //set IP
  wifi_softap_dhcps_stop(); //stop DHCP before setting static IP
  struct ip_info ip_config;
  IP4_ADDR(&ip_config.ip, 192, 168, 0, 10);
  IP4_ADDR(&ip_config.gw, 0, 0, 0, 0);
  IP4_ADDR(&ip_config.netmask, 255, 255, 255, 0);
  wifi_set_ip_info(SOFTAP_IF, &ip_config);
  wifi_softap_dhcps_start();

  // setup tcp server
  tcp_proto.local_port = 1337;
  tcp_conn.type = ESPCONN_TCP;
  tcp_conn.state = ESPCONN_NONE;
  tcp_conn.proto.tcp = &tcp_proto;
  espconn_regist_connectcb(&tcp_conn, tcp_connect_cb);
  espconn_accept(&tcp_conn);
  espconn_regist_time(&tcp_conn, 60, 0); // 60s timeout for all connections
}

#define LOOP_PRIO 2
#define QUEUE_SIZE 1
static os_event_t my_queue[QUEUE_SIZE];
void loop();

void ICACHE_FLASH_ATTR elm327_init();
void ICACHE_FLASH_ATTR st_ota_init();
void ICACHE_FLASH_ATTR uart0_init(int flashing_mode);

void ICACHE_FLASH_ATTR user_init()
{
  // init gpio subsystem
  gpio_init();

  // configure UART TXD to be GPIO1, set as output
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_GPIO1); 
  gpio_output_set(0, 0, (1 << pin), 0);

  // configure SPI
  SpiAttr hSpiAttr;
  hSpiAttr.bitOrder = SpiBitOrder_MSBFirst;
  hSpiAttr.speed = SpiSpeed_0_5MHz;
  hSpiAttr.mode = SpiMode_Master;
  hSpiAttr.subMode = SpiSubMode_0;

  // TODO: is one of these CS?
  WRITE_PERI_REG(PERIPHS_IO_MUX, 0x105);
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, 2);  // configure io to spi mode
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, 2);  // configure io to spi mode
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, 2);  // configure io to spi mode
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, 2);  // configure io to spi mode
  SPIInit(SpiNum_HSPI, &hSpiAttr);
  //SPICsPinSelect(SpiNum_HSPI, SpiPinCS_1);

  // configure UART TXD to be GPIO1, set as output
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO5_U, FUNC_GPIO5); 
  gpio_output_set(0, 0, (1 << 5), 0);
  gpio_output_set((1 << 5), 0, 0, 0);

  uart0_init(0);
  os_printf("hello\n");

  wifi_init();

  // support ota upgrades
  ota_init();
  st_ota_init();
  elm327_init();

  // jump to OS
  system_os_task(loop, LOOP_PRIO, my_queue, QUEUE_SIZE);
}


void ICACHE_FLASH_ATTR loop(os_event_t *events) {
  system_os_post(LOOP_PRIO, 0, 0);
}

