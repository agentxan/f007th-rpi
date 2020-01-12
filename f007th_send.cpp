/*
 * F007TH_push_to_rest.cpp
 *
 * Push sensors readings to remote server via REST API
 *
 * Created on: February 4, 2017
 * @author Alex Konshin <akonshin@gmail.com>
 */

#include "RFReceiver.hpp"
#include <curl/curl.h>

#include "SensorsData.hpp"
#include "Config.hpp"

static bool send(ReceivedMessage& message, Config& cfg, int changed, char* data_buffer, char* response_buffer, FILE* log);

#ifdef INCLUDE_HTTPD
static struct MHD_Daemon* start_httpd(int port, SensorsData* sensorsData);
static void stop_httpd(struct MHD_Daemon* httpd);
#endif

int main(int argc, char *argv[]) {

  if ( argc==1 ) Config::help();

  Config cfg(DEFAULT_OPTIONS);

  while (1) {
    int c = getopt_long(argc, argv, short_options, long_options, NULL);
    if (c == -1) break;

    const char* option = argv[optind];
    if (!cfg.process_cmdline_option(c, option, optarg)) {
      fprintf(stderr, "ERROR: Unknown option \"%s\".\n", option);
      Config::help();
    }
  }

  if (optind < argc) {
    if (optind != argc-1) Config::help();
    cfg.server_url = argv[optind];
  }

  if (cfg.server_url == NULL || cfg.server_url[0] == '\0') {
    if (cfg.type_is_set && cfg.server_type != STDOUT) {
      fputs("ERROR: Server URL must be specified (options --send-to or -s).\n", stderr);
      Config::help();
    }
    cfg.server_type = STDOUT;
  } else {
    //TODO support UNIX sockets for InfluxDB
    if (strncmp(cfg.server_url, "http://", 7) != 0 && strncmp(cfg.server_url, "https://", 8)) {
      fputs("ERROR: Server URL must be HTTP or HTTPS.\n", stderr);
      Config::help();
    }
  }
#ifdef TEST_DECODING
  if (cfg.input_log_file_path == NULL) {
    fputs("ERROR: Input log file must be specified (option --input-log or -I).\n", stderr);
    exit(1);
  }
#endif

  if (cfg.log_file_path == NULL || cfg.log_file_path[0]=='\0') {
#ifdef TEST_DECODING
    cfg.log_file_path = "f007th-test-decoding.log";
#else
    cfg.log_file_path = "f007th-send.log";
#endif
  }

  FILE* log = fopen(cfg.log_file_path, "w+");


  char* response_buffer = NULL;
  char* data_buffer = (char*)malloc(SEND_DATA_BUFFER_SIZE*sizeof(char));

  if (cfg.server_type!=STDOUT) {
    curl_global_init(CURL_GLOBAL_ALL);
    response_buffer = (char*)malloc(SERVER_RESPONSE_BUFFER_SIZE*sizeof(char));
  }

  SensorsData sensorsData(cfg.options);

  RFReceiver receiver(cfg.gpio);
  Log->setLogFile(log);
#ifdef TEST_DECODING
  receiver.setInputLogFile(cfg.input_log_file_path);
  receiver.setWaitAfterReading(cfg.wait_after_reading);
#endif

  if (cfg.protocols != 0) receiver.setProtocols(cfg.protocols);

  ReceivedMessage message;

  receiver.enableReceive();

#ifdef INCLUDE_HTTPD
  struct MHD_Daemon* httpd = NULL;

  if (cfg.httpd_port >= MIN_HTTPD_PORT && cfg.httpd_port<65535) {
    // start HTTPD server
    Log->log("Starting HTTPD server on port %d...", cfg.httpd_port);
    httpd = start_httpd(cfg.httpd_port, &sensorsData);
    if (httpd == NULL) {
      Log->error("Could not start HTTPD server on port %d.\n", cfg.httpd_port);
      fclose(log);
      exit(1);
    }
  }
#endif

  if ((cfg.options&VERBOSITY_PRINT_STATISTICS) != 0)
    receiver.printStatisticsPeriodically(1000); // print statistics every second

  if ((cfg.options&VERBOSITY_INFO) != 0) fputs("Receiving data...\n", stderr);
  while(!receiver.isStopped()) {

    if (receiver.waitForMessage(message)) {
      if (receiver.isStopped()) break;

      bool is_message_printed = false;

      if ((cfg.options&VERBOSITY_DEBUG) != 0 || ((cfg.options&VERBOSITY_PRINT_UNDECODED) != 0 && message.isUndecoded())) {
        message.print(stdout, cfg.options);
        is_message_printed = true;
        receiver.printManchesterBits(message, stdout);
        if (message.print(log, cfg.options)) {
          receiver.printManchesterBits(message, log);
          fflush(log);
        }
      } else if ((cfg.options&VERBOSITY_INFO) != 0) {
        message.print(stdout, cfg.options);
        is_message_printed = true;
        if (message.print(log, cfg.options)) fflush(log);
      }

      if (message.isEmpty()) {
        fputs("ERROR: Missing data.\n", stderr);
      } else if (message.isUndecoded()) {
        if ((cfg.options&VERBOSITY_INFO) != 0)
          fprintf(stderr, "Could not decode the received data (error %04x).\n", message.getDecodingStatus());
      } else {
        bool isValid = message.isValid();
        int changed = isValid ? sensorsData.update(message.getSensorData(), message.getTime(), cfg.max_unchanged_gap) : 0;
        if (changed != TIME_NOT_CHANGED) {
          if (changed == 0 && !cfg.changes_only && (isValid || cfg.server_type!=InfluxDB))
            changed = TEMPERATURE_IS_CHANGED | HUMIDITY_IS_CHANGED | BATTERY_STATUS_IS_CHANGED;
          if (changed != 0) {
            if (cfg.server_type==STDOUT) {
              if (!is_message_printed) // already printed
                message.print(stdout, cfg.options);
            } else {
              if (!send(message, cfg, changed, data_buffer, response_buffer, log) && (cfg.options&VERBOSITY_INFO) != 0)
                Log->info("No data was sent to server.");
            }
          } else {
            if ((cfg.options&VERBOSITY_INFO) != 0) {
              if (cfg.server_type==STDOUT) {
                if (!isValid)
                  fputs("Data is corrupted.\n", stderr);
                else
                  fputs("Data is not changed.\n", stderr);
              } else {
                if (!isValid)
                  fputs("Data is corrupted and is not sent to server.\n", stderr);
                else
                  fputs("Data is not changed and is not sent to server.\n", stderr);
              }
            }
          }
        }
      }

    }

    if (receiver.checkAndResetTimerEvent()) {
      receiver.printStatistics();
    }
  }

#ifdef INCLUDE_HTTPD
  if ((cfg.options&VERBOSITY_INFO) != 0) Log->log("Stopping HTTPD server...");
  stop_httpd(httpd);
#endif

  if ((cfg.options&VERBOSITY_INFO) != 0) fputs("\nExiting...\n", stderr);

  // finally
  free(data_buffer);
  if (response_buffer != NULL) free(response_buffer);
  Log->log("Exiting...");
  fclose(log);
  if (cfg.server_type!=STDOUT) curl_global_cleanup();

  exit(0);
}

typedef struct PutData {
  uint8_t* data;
  size_t len;
  uint8_t* response_data;
  size_t response_len;
  bool verbose;
} PutData;
/*
static size_t read_callback(void *ptr, size_t size, size_t nmemb, struct PutData* data) {
  size_t curl_size = nmemb * size;
  size_t to_copy = (data->len < curl_size) ? data->len : curl_size;
  memcpy(ptr, data->data, to_copy);
  if (data->verbose) fprintf(stderr, "sending %d bytes...\n", to_copy);
  data->len -= to_copy;
  data->data += to_copy;
  return to_copy;
}
*/
static size_t write_callback(void *ptr, size_t size, size_t nmemb, struct PutData* data) {
  size_t curl_size = nmemb * size;
  size_t to_copy = (data->response_len < curl_size) ? data->response_len : curl_size;
  if (data->verbose) fprintf(stderr, "receiving %d bytes...\n", (int)to_copy);
  if (to_copy > 0) {
    memcpy(data->response_data, ptr, to_copy);
    data->response_len -= to_copy;
    data->response_data += to_copy;
    return to_copy;
  }
  return size;
}

bool send(ReceivedMessage& message, Config& cfg, int changed, char* data_buffer, char* response_buffer, FILE* log) {
  bool verbose = (cfg.options&VERBOSITY_PRINT_DETAILS) != 0;
  if (verbose) fputs("===> called send()\n", stderr);

  data_buffer[0] = '\0';
  int data_size;
  if (cfg.server_type == InfluxDB) {
    data_size = message.influxDB(data_buffer, SEND_DATA_BUFFER_SIZE, changed, cfg.options);
  } else {
    data_size = message.json(data_buffer, SEND_DATA_BUFFER_SIZE, cfg.options);
  }
  if (data_size <= 0) {
    if (verbose) fputs("===> return from send() without sending because output data was not generated\n", stderr);
    return false;
  }

  struct PutData data;
  data.data = (uint8_t*)data_buffer;
  data.len = data_size;
  data.response_data = (uint8_t*)response_buffer;
  data.response_len = SERVER_RESPONSE_BUFFER_SIZE;
  data.verbose = verbose;
  response_buffer[0] = '\0';

  CURL* curl = curl_easy_init();  // get a curl handle
  if (!curl) {
    Log->error("Failed to get curl handle.", stderr);
    if (verbose) fputs("===> return from send() without sending because failed to initialize curl\n", stderr);
    return false;
  }
  if ((cfg.options&VERBOSITY_PRINT_CURL) != 0) curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

  struct curl_slist *headers = NULL;
  if (cfg.server_type == REST) {
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "charsets: utf-8");
    headers = curl_slist_append(headers, "Connection: close");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  } if (cfg.server_type == InfluxDB) {
    headers = curl_slist_append(headers, "Content-Type:"); // do not set content type
    headers = curl_slist_append(headers, "Accept:");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  curl_easy_setopt(curl, CURLOPT_URL, cfg.server_url);
//  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  if (cfg.server_type == InfluxDB)
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
  else
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
//  curl_easy_setopt(curl, CURLOPT_READFUNCTION, &read_callback);
//  curl_easy_setopt(curl, CURLOPT_READDATA, &data);
//  curl_off_t size = json_size;
//  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, size);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data_buffer);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK)
    Log->error("Sending data to %s failed: %s", cfg.server_url, curl_easy_strerror(rc));

  if (verbose && response_buffer[0] != '\0') {
    if (data.response_len <= 0) data.response_len = data.response_len-1;
    response_buffer[SERVER_RESPONSE_BUFFER_SIZE-data.response_len] = '\0';
    fputs(response_buffer, stderr);
    fputc('\n', stderr);
  }
  bool success = rc == CURLE_OK;
  long http_code = 0;
  curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code != (cfg.server_type == InfluxDB ? 204 : 200)) {
    success = false;
    if (http_code == 0)
      Log->error("Failed to connect to server %s", cfg.server_url);
    else
      Log->error("Got HTTP status code %ld.", http_code);
    if (verbose) {
      fputs(response_buffer, log);
      fputc('\n', log);
    }
    fprintf(log, "ERROR: Got HTTP status code %ld.\n", http_code);
    fputs(response_buffer, log);
    fputc('\n', log);
  } else if (rc == CURLE_ABORTED_BY_CALLBACK) {
    Log->error("HTTP request was aborted.");
  }
  curl_easy_cleanup(curl);
  if (headers != NULL) curl_slist_free_all(headers);
  if (verbose) fputs("===> return from send()\n", stderr);
  return success;
}

#ifdef INCLUDE_HTTPD
/*
 * Handling HTTP requests
 */

static int ahc_echo(
    void* cls,
    struct MHD_Connection * connection,
    const char* url,
    const char* method,
    const char* version,
    const char* upload_data,
    size_t* upload_data_size,
    void** ptr
) {
  static int dummy;
  SensorsData* sensorsData = (SensorsData*)cls;
  struct MHD_Response* response;
  int ret;

  if (strcmp(method, "GET") != 0) return MHD_NO; /* unexpected method */
  if (&dummy != *ptr) {
    /* The first time only the headers are valid, do not respond in the first round... */
    *ptr = &dummy;
    return MHD_YES;
  }
  if (0 != *upload_data_size) return MHD_NO; /* upload data in a GET!? */

  *ptr = NULL; /* clear context pointer */
  void* buffer = __null;
  size_t buffer_size = 0;

  size_t data_size = sensorsData->generateJson(buffer, buffer_size);
  if (data_size == 0)
    response = MHD_create_response_from_buffer(2, (void*)"[]", MHD_RESPMEM_PERSISTENT);
  else
    response = MHD_create_response_from_buffer(data_size, buffer, MHD_RESPMEM_MUST_FREE);
  MHD_add_response_header(response, "Content-Type", "application/json");

  ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
  MHD_destroy_response(response);
  return ret;
}

struct MHD_Daemon* start_httpd(int port, SensorsData* sensorsData) {
  struct MHD_Daemon* httpd;
  httpd =
      MHD_start_daemon(
          MHD_USE_THREAD_PER_CONNECTION,
          port,
          NULL,
          NULL,
          &ahc_echo,
          (void*)sensorsData,
          MHD_OPTION_END
      );

  return httpd;
}

void stop_httpd(struct MHD_Daemon* httpd) {
  MHD_stop_daemon(httpd);
}

#endif


