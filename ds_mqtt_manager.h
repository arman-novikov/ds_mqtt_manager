#ifndef DS_MQTT_MANAGER
#define DS_MQTT_MANAGER

#include <ds_console.h>
#include <Ethernet.h>
#include <PubSubClient.h>
#include <Arduino.h>
#include <avr/wdt.h>

/*!
* @file contains class MQTT_manager, types and values
*       to operate props using MQTT protocol
*/
constexpr char MQTT_STRSTATUS_READY[]    = "Not activated"; // "Ready"?
constexpr char MQTT_STRSTATUS_ENABLED[]  = "Activated";
constexpr char MQTT_STRSTATUS_FINISHED[] = "Finished";

constexpr size_t PROP_STATUS_MAX_SIZE       = 16U;
constexpr size_t PROP_CB_TYPES_NUM          = 3U; // onActivate, onFinish, onReset

enum prop_cb_types { MQTT_CB_ACTIVATE, MQTT_CB_FINISH, MQTT_CB_RESET };
typedef void(*prop_CBs_t[PROP_CB_TYPES_NUM])(void);
typedef prop_CBs_t* props_CBs_t;
typedef char prop_state_t[PROP_STATUS_MAX_SIZE];
typedef char *const props_states_t;

struct ds_MQTT {
  static void reset()
  {
    wdt_enable(WDTO_60MS);
    delay(1000);
  }
  static constexpr int8_t NOT_SHOW = -1;
  typedef void (*mqtt_msg_handler_t)(char*, uint8_t*, unsigned int);
};

/*!
* @class MQTT_manager 
* @brief Facade class to use mqtt protocol via ethernet
* @param [in] size_t props_count number of props operating on the circuit
* @param [in] CLIENT_NAME unique id of the circuit
* @param [in] props_STRIDS array of props' names
* @param [in] er_onStart procedure called on ERP Start Game cmd
* @param [in] er_onReset procedure called on ERP Reset All cmd
* @param [in] props_CBs array of each prop callbacks (onActivate, OnFinish, onReset)
* @param [in] special_CB pointer to a procedure to process a mqtt_msg in a custom way
* @warning props_CBs has to contatin props_count arrays
           of each prop's callbacks (3 CBs for each prop)
* @todo reorder data fields for memory align
* @example
*            Console *consOLE = new Console();
*
*            constexpr size_t PROPS_NUM = 3;
*            constexpr char strID[] = "box_yammychoco_mokka_EK$$$";
*
*            const char rname1[] = "box"; //> will be shown in ERP as "Box"
*            const char rname2[] = "yammy_choco"; //> will be shown in ERP as "Yammy choco"
*            const char rname3[] = "_mokka"; // not to be shown in the ERP because of prefix '_'
*            const char *propsNames[PROPS_NUM] = {rname1, rname2, rname3};
*            constexpr int riddles_num_in_ERP[PROPS_NUM] = {2,5,8};
*
*            void onSrt() {} void onRst() {}
*            void r1a() {} void r1f() {} void r1r() {}
*            void r2a() {} void r2f() {} void r2r() {}
*            void r3a() {} void r3f() {} void r3r() {}
*            void my_special_cb(char* topic, uint8_t* payload, unsigned int len)
*            { if (strcmp(topic, "/er/music/cmd")) do_something(); }
*
*            prop_CBs_t prop1 = { r1a, r1f, r1r };
*            prop_CBs_t prop2 = { r2a, r2f, r2r };
*            prop_CBs_t prop3 = { r3a, r3f, r3r };
*            props_CBs_t props_cbs[PROPS_NUM] = { &prop1, &prop2, &prop3};
*
*
*            prop_state_t boxState   = {0};
*            prop_state_t chocoState = {0};
*            prop_state_t mokkaState = {0};
*            props_states_t props_states[PROPS_NUM] = {boxState, chocoState, mokkaState};
*
*            ...
*            auto *manag = new MQTT_manager< PROPS_NUM, strID, propsNames, riddles_num_in_ERP,
*                                            onSrt, onRst, props_cbs>(consOLE);
*            ...
*            manag->check();
*            ...
*            manag->publish("hi", "there");
*            ...
*            manag->sendInfoLoop(props_states);
*            ...
*/
template<size_t props_count,
         const char* CLIENT_NAME,     /// client_name is for PROP_NAME
         const char** props_STRIDS,   /// for STRIDs
         const int* mqtt_numbers,     /// for props' numbers in ERP
         void (*er_onStart)(),
         void (*er_onReset)(),
         props_CBs_t *props_CBs,
         void (*special_CB)(char*, uint8_t*, unsigned int) = nullptr,
         const char** extra_topics = nullptr,
         const size_t extra_topics_count = 0>
class MQTT_manager
{
public:
/*!
* @brief MQTT_manager constructor
* @detail setup a client, server, callback for received msgs
* @param [in] console pointer to out stream Strategy object
* @param [in] ip_ending necessary for Ethernet static object (Singleton)
* @param [in] mqqt_port server port for PubSubClient (this class' field)
* @todo shrink it
* @todo replace the hardcode
*/
  explicit MQTT_manager(const Console *console,
                        const byte ip_ending,
                        const size_t &mqtt_port = 1883):
    _console(console),
    _server(192, 168, 10, 1),
    _lastReconnectAttempt(0),
    _ip_ending(ip_ending)
  {
    _console->println(F("Initializing Ethernet..."));
    _startEthernet();
    _console->print(F("Local IP: "));
    _console->println(Ethernet.localIP());
    _console->println(F("Ethernet Initialized..."));
    _client.setClient(_ethernetClient);
    _client.setServer(_server, mqtt_port);
    _client.setCallback(default_msg_handler);
    delay(1500);
  }

/*!
* @brief a procedure to be called in loop
* @param props_states props' current states
* @warning props_states' elements' number must be equal to props_count
* @detail calls methods: _check and _sendInfoLoop
*/
  void routine(const char *const *props_states)
  {
    _check();
    _sendInfoLoop(props_states);
  }

/*!
* @brief decorator providing access to mqtt publish interface
* @param [in] topic kind of address
* @param [in] payload the msg itself
* @param [in] retained if true the value is supposed to be kept on mqtt server
* @return bool true if published and false otherwise
*/
  bool publish(const char* topic, const char* payload, bool retained = false)
  {
    return _client.publish(topic, payload, retained);
  }

  bool is_connected()
  {
    return _client.connected();
  }

  MQTT_manager(const MQTT_manager&)             = delete;
  MQTT_manager(MQTT_manager&&)                  = delete;
  MQTT_manager& operator=(const MQTT_manager&)  = delete;
  MQTT_manager& operator=(MQTT_manager&&)       = delete;

private:
  static constexpr size_t BUF_SIZE                   = 128U;
  static constexpr size_t ON_CONNECTED_BUF_MAX_SIZE  = 32U;
  static void default_msg_handler(char* topic, uint8_t* payload, unsigned int length);
/*!
* @brief makes hardware checks
* @return zero on success otherwise error code
* @detail check ethernet module and cable availbility         
* @todo DRY it, but remain memory usage amount
*/
  int _hardware_status()
  {
    static bool last_status = true;
    static unsigned long last_time_stamp = millis();
        
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      if (millis() - last_time_stamp > 1000) {
        _console->println(F("ethernet module missing"));
        last_time_stamp = millis();
      }
      last_status = false;
      return -1;
    }

    if (Ethernet.linkStatus() == LinkOFF) {
      if (millis() - last_time_stamp > 1000) {
        _console->println(F("LAN cable missing"));
        last_time_stamp = millis();
      }
      last_status = false;
      return -1;
    }

    if (last_status == false)
      _console->println(F("ethernet hardware is restored"));
    
    last_status = true;
    return 0;
  }

/*!
* @brief does mqtt routine if connected
*         else tries to connect
*/
  void _check()
  {
    if (_hardware_status())
      return;
      
    if ( _client.connected() ) {
      _client.loop();           /// does mqtt routine
      return;
    }

    unsigned long now = millis();             /// every 5 seconds
    if (now - _lastReconnectAttempt > 5000) {
      _lastReconnectAttempt = now;
      if (this->_reconnect())                /// tries to reconnect
        _lastReconnectAttempt = 0;
    }
  }

/*!
* @brief publishes info about props' props states every second,
*        also, kind of a heartbeat system
* @param props_states props' current states
* @warning props_states' elements' number must be equal to props_count
* @detail for prop name in ERP all '_' are replaced with ' '
            and the 1st letter to upper case
* @todo avoid using of var "riddleStrName"
*/
  void _sendInfoLoop(const char *const *props_states)
  {
    static unsigned long lastTS = 0;
    if (millis() - lastTS <= 1000)
      return;    

    for (size_t i = 0; i < props_count; ++i) {
      char msgBuf[BUF_SIZE] = {0};
      
      if (props_STRIDS[i] == nullptr) /// < means no need to public in ERP
        continue;

      if (props_STRIDS[i][0] == '_' || mqtt_numbers[i] < 0) /// < todo: delete '_'
        continue;

      _msgInfo(msgBuf, // input param
               props_STRIDS[i],
               props_states[i],
               mqtt_numbers[i]);

      this->publish("/er/riddles/info", msgBuf);
    }

    lastTS = millis();
  }

/*!
* @brief tries to reconnect to mqqt server
* @return bool true if reconnected and false otherwise
*/
  bool _reconnect()
  {
    _console->println(F("MQTT: Connecting ..."));

    if (_client.connect(CLIENT_NAME)) {
      _console->print(F("MQTT: Connected (id: "));
      _console->print(CLIENT_NAME);
      _console->println(F(")"));
      _onConnected();
    } else {
      _console->print(F("MQTT: Failed, Return Code: "));
      _console->println(_client.state());
      _onDisconnected();
    }

    return _client.connected();
  }

/*!
* @brief does a mqtt client connection routines
* @detail subscribes to topics "/er/cmd" and the props' ones
*/
  void _onConnected()
  {
    for (size_t i = 0; i < props_count; ++i) {
      char buf[ON_CONNECTED_BUF_MAX_SIZE] = {"/er/"};
      strcat(buf, props_STRIDS[i]);
      strcat(buf, "/cmd");
      _client.subscribe(buf);
    }

    _client.subscribe("/er/cmd");

    for (size_t i = 0; i < extra_topics_count; ++i)
      _client.subscribe(extra_topics[i]);
  }
  
/*!
* @brief simply restarts the W5500 object
*/
  void _startEthernet()
  {
    byte mac[] = {0x90, 0xA2, 0xDA, 0x10, 0xA9, _ip_ending};
    IPAddress ip(192, 168, 10, _ip_ending);

    Ethernet.begin(mac, ip);
  }

  /*!
  * @brief method to be called if the connection lost
  * @todo: improve fault-tolerance
  */

  void _onDisconnected()
  {
    _startEthernet();
  }

/*!
* @brief "builds" a string according to the customized mqtt protocol
* @param [out] msgData result of the procedure
* @param [in] strId prop id name
* @param [in] strStatus prop's current state
* @param [in] number prop's number in ERP
* @detail if strId[0] == '_' the riddle not to be shown in the ERP
*/
  static void _msgInfo(char *msgData,
                const char* strId,
                const char* strStatus,
                const int &number)
  {
    //"{\"strId\":\"" MQTT_1_STRID "\", \"strName\":\"" MQTT_1_STRNAME "\", \"strStatus\":\"" + strStatus1 + "\", \"number\":\"" + MQTT_1_NUMBER + "\"}";
	char *spacer_ptr;
	size_t start, end;
	msgData[0] = 0;
	strcat(msgData, "{");

	//  strId  //
	strcat(msgData, "\"strId\":\"");
	strcat(msgData, strId);
	strcat(msgData, "\", ");

	//  strName  //
	strcat(msgData, "\"strName\":\"");
	end = start = strlen(msgData);
	strcat(msgData, strId);
	end += strlen(strId);
	spacer_ptr = msgData + start;
	while (spacer_ptr < msgData + end) { // all '_'s into ' 's
		if (*spacer_ptr == '_')
			*spacer_ptr = ' ';
		++spacer_ptr;
	}
	msgData[start] -= 32; // capitalizing the 1st letter
	strcat(msgData, "\", ");

	//  strStatus  //
	strcat(msgData, "\"strStatus\":\"");
	strcat(msgData, strStatus);
	strcat(msgData, "\", ");

	//  number  //
	strcat(msgData, "\"number\":\"");
	char strVal2[8];
	itoa(number, strVal2, 10);
	strcat(msgData, strVal2);
	strcat(msgData, "\"");

	//  end  //
	strcat(msgData, "}");
}

  const Console   *_console;
  IPAddress       _server;
  PubSubClient    _client;
  EthernetClient  _ethernetClient;
  unsigned long   _lastReconnectAttempt;
  const byte      _ip_ending;
};


template<size_t props_count,
         const char* CLIENT_NAME,
         const char** props_STRIDS,
         const int* mqtt_numbers,
         void (*er_onStart)(),
         void (*er_onReset)(),
         props_CBs_t *props_CBs,
         void (*special_CB)(char*, uint8_t*, unsigned int),
         const char** extra_topics,
         const size_t extra_topics_count
>  void MQTT_manager<
  props_count, CLIENT_NAME, props_STRIDS,
  mqtt_numbers, er_onStart, er_onReset, props_CBs,
  special_CB, extra_topics, extra_topics_count>::default_msg_handler
    (char* topic, uint8_t* payload, unsigned int length) 
{
  char* payloadStr = reinterpret_cast<char*>(payload);
    payloadStr[length] = {0};
    
    for (size_t i = 0; i < props_count; ++i) {
      char buf[32] = {"/er/"};                  /// !!! HARDCODE !!!
      strcat(buf, props_STRIDS[i]);
      strcat(buf, "/cmd");

      if (strcmp(topic, buf) != 0 || props_CBs[i] == nullptr)
        continue;

      if (strcmp(payloadStr, "activate") == 0) {
        if((*props_CBs[i])[MQTT_CB_ACTIVATE])
          (*props_CBs[i])[MQTT_CB_ACTIVATE]();
        return;
      } else if (strcmp(payloadStr, "finish") == 0) {
        if((*props_CBs[i])[MQTT_CB_FINISH])
          (*props_CBs[i])[MQTT_CB_FINISH]();
        return;
      } else if (strcmp(payloadStr, "reset") == 0) {
        if((*props_CBs[i])[MQTT_CB_RESET])
          (*props_CBs[i])[MQTT_CB_RESET]();
        return;
      }
    }

    if (strcmp(topic, "/er/cmd") == 0) {
      if (strcmp(payloadStr, "start") == 0) {
        er_onStart();
        return;
      }                                

      if (strcmp(payloadStr, "reset") == 0) {
        er_onReset();
        return;
      }                                
    }
  #pragma GCC diagnostic ignored "-Waddress"
    if (special_CB)
  #pragma GCC diagnostic pop
      special_CB(topic, payload, length);

    memset(payloadStr, 0, length); // todo: delete it  
}

#endif