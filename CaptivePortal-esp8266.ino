
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <stdio.h>
#include <string.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <Hash.h>

#include "DNSServer.h"

extern "C"
{
#include "user_interface.h"

	void system_set_os_print ( uint8 onoff );
	void ets_install_putc1 ( void *routine );
}

ADC_MODE ( ADC_VCC ); 															// Set ADC for Voltage Monitoring

// Use the internal hardware buffer
static void _u0_putc ( char c )
{
	while ( ( ( U0S >> USTXC ) & 0x7F ) == 0x7F );

	U0F = c;
}

//***************************************************************************
// Global data section.														*
//***************************************************************************
char  				ssid[] = "SbiHotspot";
bool		 		DEBUG = 1;

// Maximum number of simultaneous clients connected (WebSocket)
#define MAX_WS_CLIENT   5

#define CLIENT_NONE     0
#define CLIENT_ACTIVE   5

int counter =0;
// Web Socket client state
typedef struct
{
	uint32_t  id;
	uint8_t   state;
} _ws_client;

IPAddress			ip ( 10, 10, 10, 1 );                  						// Private network for httpd
DNSServer			dnsd;                            							// Create the DNS object

AsyncWebServer		httpd ( 80 );                      							// Instance of embedded webserver
AsyncWebSocket		ws ( "/ws" );                        						// access at ws://[esp ip]/ws
_ws_client			ws_client[MAX_WS_CLIENT];               					// State Machine for WebSocket Client;

int					rrcount;													
char 				str_vcc[8];
const char* fp = "44 40 9E 34 92 2D E4 61 A4 89 A8 D5 7F 71 B7 62 B3 FD DD E1";

//***************************************************************************
// End of global data section.												*
//***************************************************************************

//***************************************************************************
//                            D B G P R I N T								*
//***************************************************************************
void dbg_printf ( const char *format, ... )
{
	static char sbuf[1400] ;               										// For debug lines
	va_list varArgs ;                                    						// For variable number of params

	va_start ( varArgs, format ) ;                      						// Prepare parameters
	vsnprintf ( sbuf, sizeof ( sbuf ), format, varArgs ) ;  					// Format the message
	va_end ( varArgs ) ;                                 						// End of using parameters

	Serial.println ( sbuf ) ;

}

//***************************************************************************
//                    F O R M A T  B Y T E S			       	    		*
//***************************************************************************
String formatBytes ( size_t bytes )
{
	if ( bytes < 1024 )
	{
		return String ( bytes ) + " B";
	}
	else if ( bytes < ( 1024 * 1024 ) )
	{
		return String ( bytes / 1024.0 ) + " KB";
	}
	else if ( bytes < ( 1024 * 1024 * 1024 ) )
	{
		return String ( bytes / 1024.0 / 1024.0 ) + " MB";
	}
	else
	{
		return String ( bytes / 1024.0 / 1024.0 / 1024.0 ) + " GB";
	}
}

//***************************************************************************
//                    S E T U P												*
//***************************************************************************
void setup ( void )
{
	uint8_t		mac[6];

	ets_install_putc1 ( ( void * ) &_u0_putc );
	system_set_os_print ( 1 );
  system_update_cpu_freq ( 160 ) ;                      						// Set CPU to 80/160 MHz

	Serial.begin ( 115200 ) ;                         							// For debug
	Serial.println() ;



	// Setup Access Point
	setupAP();
	WiFi.softAPmacAddress ( mac );

	dbg_printf ( "SYSTEM ---" );
	dbg_printf ( "getSdkVersion:      %s", ESP.getSdkVersion() );
	dbg_printf ( "getBootVersion:     %08X", ESP.getBootVersion() );
	dbg_printf ( "getBootMode:        %08X", ESP.getBootMode() );
	dbg_printf ( "getChipId:          %08X", ESP.getChipId() );
	dbg_printf ( "getCpuFreqMHz:      %d Mhz", ESP.getCpuFreqMHz() );
	dbg_printf ( "getCycleCount:      %u\n", ESP.getCycleCount() );

	dbg_printf ( "POWER ---" );
	dbg_printf ( "Voltage:            %d.%d v\n ", (ESP.getVcc() / 1000), (ESP.getVcc() % 1000) );

	dbg_printf ( "MEMORY ---" );
	dbg_printf ( "getFreeHeap:        %d B", ESP.getFreeHeap() );
	dbg_printf ( "getSketchSize:      %s", formatBytes ( ESP.getSketchSize() ).c_str() );
	dbg_printf ( "getFreeSketchSpace: %s", formatBytes ( ESP.getFreeSketchSpace() ).c_str() );
	dbg_printf ( "getFlashChipSize:   %s", formatBytes ( ESP.getFlashChipRealSize() ).c_str() );
	dbg_printf ( "getFlashChipSpeed:  %d MHz\n", int ( ESP.getFlashChipSpeed() / 1000000 ) );

	setupSPIFFS();

	// Setup DNS Server
	// if DNS Server is started with "*" for domain name,
	// it will reply with provided IP to all DNS request
	dbg_printf ( "Starting DNS Server" ) ;
	dnsd.start ( 53, "*", ip );
  httpd.onNotFound ( onRequest ) ;       
	setupHTTPServer();
	setupOTAServer();

	dbg_printf ( "\nReady!\n--------------------" ) ;

}

void setupAP()
{
	WiFi.mode ( WIFI_AP );
	WiFi.softAPConfig ( ip, ip, IPAddress ( 255, 255, 255, 0 ) );
	WiFi.softAP ( ssid );
}

void setupSPIFFS()
{
	FSInfo		fs_info ;                       								// Info about SPIFFS
	Dir			dir ;                         									// Directory struct for SPIFFS
	File		f ;                           									// Filehandle
	String		filename ;                        								// Name of file found in SPIFFS

	SPIFFS.begin() ;                              								// Enable file system

	// Show some info about the SPIFFS
	uint16_t cnt = 0;
	SPIFFS.info ( fs_info ) ;
	dbg_printf ( "SPIFFS Files\nName                           -      Size" );
	dir = SPIFFS.openDir ( "/" ) ;                      						// Show files in FS

	while ( dir.next() )                            							// All files
	{
		f = dir.openFile ( "r" ) ;
		filename = dir.fileName() ;
		dbg_printf ( "%-30s - %9s",                      						// Show name and size
					 filename.c_str(),
					 formatBytes ( f.size() ).c_str()
				   ) ;
		cnt++;
	}

	dbg_printf ( "%d Files, %s of %s Used",
				 cnt,
				 formatBytes ( fs_info.usedBytes ).c_str(),
				 formatBytes ( fs_info.totalBytes ).c_str()
			   );
	dbg_printf ( "%s Free\n",
				 formatBytes ( fs_info.totalBytes - fs_info.usedBytes ).c_str()
			   );
}

void setupHTTPServer()
{
	// Web Server Document Setup
	dbg_printf ( "Starting HTTP Captive Portal" ) ;

	httpd.on ( "/", HTTP_GET, onRequest ) ;                  					// Handle startpage
	httpd.onNotFound ( onRequest ) ;                     						// Handle file from FS

  
	httpd.on ( "/trigger", HTTP_GET, [] ( AsyncWebServerRequest * request )
	{
		AsyncWebHeader *h = request->getHeader ( "User-Agent" );

		rrcount++;
		IPAddress remoteIP = request->client()->remoteIP();
		request->send ( 200, "text/html", String ( rrcount ) ) ;
	} );


	// attach AsyncWebSocket
	dbg_printf ( "Starting Websocket Console" ) ;
	ws.onEvent ( onEvent );
	httpd.addHandler ( &ws );

	httpd.begin() ;
}

void setupOTAServer()
{
	dbg_printf ( "Starting OTA Update Server" ) ;

	// Port defaults to 8266
	// ArduinoOTA.setPort(8266);

	// Hostname defaults to esp8266-[ChipID]
	ArduinoOTA.setHostname("FreeeWiFi");

	// No authentication by default
	// ArduinoOTA.setPassword((const char *)"123");

	// OTA callbacks
	ArduinoOTA.onStart ( []()
	{
		ws.enable ( false );													// Disable client connections
		dbg_printf ( "OTA Update Started" );									// Let connected clients know what's going on
	} );
	ArduinoOTA.onEnd ( []()
	{
		dbg_printf ( "OTA Update Complete!\n" );

		if ( ws.count() )
		{
			ws.closeAll();  														// Close connected clients
			delay ( 1000 );
		}

	} );
	ArduinoOTA.onProgress ( [] ( unsigned int progress, unsigned int total )
	{
		dbg_printf ( "Progress: %u%%\r", ( progress / ( total / 100 ) ) );
	} );
	ArduinoOTA.onError ( [] ( ota_error_t error )
	{
		dbg_printf ( "Error[%u]: ", error );

		if ( error == OTA_AUTH_ERROR ) dbg_printf ( "Auth Failed" );
		else if ( error == OTA_BEGIN_ERROR ) dbg_printf ( "Begin Failed" );
		else if ( error == OTA_CONNECT_ERROR ) dbg_printf ( "Connect Failed" );
		else if ( error == OTA_RECEIVE_ERROR ) dbg_printf ( "Receive Failed" );
		else if ( error == OTA_END_ERROR ) dbg_printf ( "End Failed" );
	} );
	ArduinoOTA.begin();
}

//***************************************************************************
//                    L O O P												*
//***************************************************************************
// Main program loop.                                                       *
//***************************************************************************
void loop ( void )

{
	dnsd.processNextRequest();
  delay(1000);
  if(counter>100){
   dbg_printf ( "restart" ) ;
    ws.closeAll() ;
    ESP.restart() ;
    counter=0;
}
   counter++;
 Serial.println(counter);
 ArduinoOTA.handle();  // Handle remote Wifi Updates
}

//***************************************************************************
// HTTPD onRequest															*
//***************************************************************************
void onRequest ( AsyncWebServerRequest *request )
{
	digitalWrite ( LED_BUILTIN, LOW );                    						// Turn the LED on by making the voltage LOW

	IPAddress remoteIP = request->client()->remoteIP();
	dbg_printf (
		"HTTP[%d]: %s%s",
		remoteIP[3],
		request->host().c_str(),
		request->url().c_str()
	) ;

	String path = request->url();

	if ( path.endsWith ( "/" ) )
		path += "index.html";

	if ( !SPIFFS.exists ( path ) && !SPIFFS.exists ( path + ".gz" ) )
	{
		//AsyncWebHeader *h = request->getHeader ( "User-Agent" );
		// Redirect to captive portal
	//	dbg_printf ( "HTTP[%d]: Redirected to captive portal\n%s", remoteIP[3], h->value().c_str() ) ;
		request->redirect ( "http://www.SbiHotspot.com/index.html" );
	}
	else
	{
		if ( !request->hasParam ( "download" ) && SPIFFS.exists ( path + ".gz" ) )
		{
			request->send ( SPIFFS, path, String(), request->hasParam ( "download" ) );
		}
		else
		{
			request->send ( SPIFFS, path ) ;                						// Okay, send the file
		}
	}
	digitalWrite ( LED_BUILTIN, HIGH );                   						// Turn the LED off by making the voltage HIGH
}


//***************************************************************************
// WebSocket onEvent														*
//***************************************************************************
// Manage routing of websocket events                                       *
//***************************************************************************
void onEvent ( AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len )
{
	if ( type == WS_EVT_CONNECT )
	{
		uint8_t index;
		//dbg_printf ( "ws[%s][%u] connect\n", server->url(), client->id() );

		for ( index = 0; index < MAX_WS_CLIENT ; index++ )
		{
			if ( ws_client[index].id == 0 )
			{
				ws_client[index].id = client->id();
				ws_client[index].state = CLIENT_ACTIVE;
				//dbg_printf ( "added #%u at index[%d]\n", client->id(), index );
				client->printf ( "[[b;green;]Hello Client #%u, added you at index %d]", client->id(), index );
				client->ping();
				break; // Exit for loop
			}
		}

		if ( index >= MAX_WS_CLIENT )
		{
			dbg_printf ( "not added, table is full" );
		}
	}
	else if ( type == WS_EVT_DISCONNECT )
	{
		dbg_printf ( "ws[%s][%u] disconnect: %u\n", server->url(), client->id() );

		for ( uint8_t i = 0; i < MAX_WS_CLIENT ; i++ )
		{
			if ( ws_client[i].id == client->id() )
			{
				ws_client[i].id = 0;
				ws_client[i].state = CLIENT_NONE;
				dbg_printf ( "freed[%d]\n", i );
				break; // Exit for loop
			}
		}
	}
	else if ( type == WS_EVT_ERROR )
	{
		dbg_printf ( "WS[%u]: error(%u) - %s", client->id(), * ( ( uint16_t * ) arg ), ( char * ) data );
	}
	else if ( type == WS_EVT_DATA )
	{
		//data packet
		AwsFrameInfo *info = ( AwsFrameInfo * ) arg;
		char *msg = NULL;
		size_t n = info->len;
		uint8_t index;
		// Size of buffer needed
		// String same size +1 for \0
		// Hex size*3+1 for \0 (hex displayed as "FF AA BB ...")
		n = info->opcode == WS_TEXT ? n + 1 : n * 3 + 1;
		msg = ( char * ) calloc ( n, sizeof ( char ) );

		if ( msg )
		{
			// Grab all data
			for ( size_t i = 0; i < info->len; i++ )
			{
				if ( info->opcode == WS_TEXT )
				{
					msg[i] = ( char ) data[i];
				}
				else
				{
					sprintf_P ( msg + i * 3, PSTR ( "%02x " ), ( uint8_t ) data[i] );
				}
			}
		}

		//dbg_printf ( "ws[%s][%u] message %s\n", server->url(), client->id(), msg );

		// Search if it's a known client
		for ( index = 0; index < MAX_WS_CLIENT ; index++ )
		{
			if ( ws_client[index].id == client->id() )
			{
				//dbg_printf ( "known[%d] '%s'\n", index, msg );
				//dbg_printf ( "client #%d info state=%d\n", client->id(), ws_client[index].state );

				// Received text message
				if ( info->opcode == WS_TEXT )
				{
					execCommand ( client, msg );
				}
				else
				{
					dbg_printf ( "Binary 0x:%s", msg );
				}

				// Exit for loop
				break;
			} // if known client
		} // for all clients

		// Free up allocated buffer
		if ( msg )
			free ( msg );
	} // EVT_DATA
}

//***************************************************************************
// WebSocket execCommand													*
//***************************************************************************
// translate and execute command                                            *
//***************************************************************************
void execCommand ( AsyncWebSocketClient * client, char * msg )
{
	uint16_t l = strlen ( msg );
	uint8_t index = MAX_WS_CLIENT;

	// Search if we're known client
	if ( client )
	{
		for ( index = 0; index < MAX_WS_CLIENT ; index++ )
		{
			// Exit for loop if we are there
			if ( ws_client[index].id == client->id() )
				break;
		} // for all clients
	}
	else
	{
		return;
	}

	// Custom command to talk to device
	if ( !strcasecmp_P ( msg, PSTR ( "ping" ) ) )
	{
		client->printf_P ( PSTR ( "received your [[b;cyan;]ping], here is my [[b;cyan;]pong]" ) );

	}
	else if ( !strcasecmp_P ( msg, PSTR ( "debug" ) ) )
	{
		DEBUG = !DEBUG;
		if ( DEBUG )
		{
			client->printf_P ( PSTR ( "[[b;green;]Debug output enabled]" ) );
		}
		else
		{
			client->printf_P ( PSTR ( "[[b;red;]Debug output disabled]" ) );
		}

	}
	// Dir files on SPIFFS system
	// --------------------------
	else if ( !strcasecmp_P ( msg, PSTR ( "ls" ) ) )
	{
		FSInfo fs_info ;
		uint16_t cnt = 0;
		String filename;
		Dir dir = SPIFFS.openDir ( "/" ) ;                    					// Show files in FS
		while ( dir.next() )                            						// All files
		{
			cnt++;
			File f = dir.openFile ( "r" ) ;
			filename = dir.fileName() ;
			f.close();
		}

		SPIFFS.info ( fs_info ) ;
	}
	
	else if ( !strcasecmp_P ( msg, PSTR ( "who" ) ) )
	{
		uint8_t cnt = 0;

		// Count client
		for ( uint8_t i = 0; i < MAX_WS_CLIENT ; i++ )
		{
			if ( ws_client[i].id )
			{
				cnt++;
			}
		}

	}
	else if ( !strcasecmp_P ( msg, PSTR ( "info" ) ) )
	{
		uint8_t mac[6];
		WiFi.softAPmacAddress ( mac );
	}
	
	else if ( ( l > 5 && !strncasecmp_P ( msg, PSTR ( "ssid " ), 5 ) )  )
	{
		sprintf ( ssid, "%s", &msg[5] );
		dbg_printf ( "[[b;yellow;]Changing SSID:] %s" , ssid );
		setupAP();
	}
	
	else if ( !strcasecmp_P ( msg, PSTR ( "reboot" ) ) )
	{
		ws.closeAll() ;
		delay ( 1000 ) ;
		ESP.restart() ;
	}

	dbg_printf ( "WS[%d]: %s", client->id(), msg );
}


