#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define DS_DA_IMPLEMENTATION
#define DS_SB_IMPLEMENTATION
#define DS_SS_IMPLEMENTATION
#define DS_IO_IMPLEMENTATION
#define DS_AP_IMPLEMENTATION
#include "ds.h"

/*
 * Maximum amount of data read from one HTTP request.
 *
 * This is still a simple educational server, so requests larger
 * than MAX_LEN are not fully supported.
 */
#define MAX_LEN 1024

/* Maximum number of pending connections in the listen queue. */
#define MAX_LISTEN 10

/* HTTP response bodies used for errors. */
#define NOT_FOUND_STR "Not found"
#define BAD_REQUEST_STR "Bad request"
#define METHOD_NOT_ALLOWED_STR "Method not allowed"
#define INTERNAL_ERROR_STR "Internal server error"
#define CREATED_STR "Created"
#define DELETED_STR "Deleted"

/*
 * Supported HTTP methods.
 *
 * Originally the server only supported GET.
 * We now support:
 *
 *     GET
 *     POST
 *     PUT
 *     DELETE
 */
typedef enum request_kind { GET, POST, PUT, DELETE } request_kind;

/*
 * Supported HTTP protocol versions.
 */
typedef enum protocol_kind { HTTP_1_1 } protocol_kind;

/*
 * Represents one parsed HTTP request.
 *
 * Example:
 *
 *     POST /hello.txt HTTP/1.1
 *
 *     Content-Length: 5
 *
 *     Hello
 *
 * becomes approximately:
 *
 *     kind       = POST
 *     path       = "/hello.txt"
 *     protocol   = "HTTP/1.1"
 *     body       = "Hello"
 *     body_len   = 5
 */
typedef struct request {
  request_kind kind;

  char *path;
  char *protocol;

  /*
   * Pointer to the request body.
   *
   * This points inside the original read buffer.
   * We therefore do NOT free it separately.
   */
  char *body;

  /*
   * Number of bytes in the request body.
   */
  int body_len;
} request_t;

/*
 * One HTTP header.
 *
 * Example:
 *
 *     Content-Type: text/plain
 *
 * becomes:
 *
 *     key   = "Content-Type"
 *     value = "text/plain"
 */
typedef struct header {
  char *key;
  char *value;
} header_t;

/*
 * Represents an HTTP response.
 */
typedef struct response {
  protocol_kind protocol;
  int status_code;

  /*
   * Dynamic array containing header_t objects.
   */
  ds_dynamic_array headers;

  /*
   * Response body.
   */
  char *content;
} response_t;

/*
 * Convert our internal protocol enum into the actual HTTP text.
 */
const char *protocol_kind_serialize(protocol_kind kind) {
  switch (kind) {
  case HTTP_1_1:
    return "HTTP/1.1";
  }

  return "";
}

/*
 * Convert the dynamic array of headers into a single string.
 *
 * Example:
 *
 *     Content-Type: text/plain
 *     Content-Length: 5
 */
const char *headers_serialize(ds_dynamic_array *headers) {
  char *buffer = NULL;

  ds_string_builder buffer_builder;
  ds_string_builder_init(&buffer_builder);

  for (size_t i = 0; i < headers->count; i++) {
    header_t header;

    ds_dynamic_array_get(headers, i, &header);

    ds_string_builder_append(&buffer_builder, "%s: %s\r\n", header.key,
                             header.value);
  }

  ds_string_builder_build(&buffer_builder, &buffer);

  return buffer;
}

/*
 * Convert HTTP status codes into HTTP status strings.
 */
const char *status_code_serialize(int status_code) {
  if (status_code == 200) {
    return "200 OK";
  }

  if (status_code == 201) {
    return "201 Created";
  }

  if (status_code == 204) {
    return "204 No Content";
  }

  if (status_code == 400) {
    return "400 Bad Request";
  }

  if (status_code == 404) {
    return "404 Not Found";
  }

  if (status_code == 405) {
    return "405 Method Not Allowed";
  }

  if (status_code == 409) {
    return "409 Conflict";
  }

  if (status_code == 500) {
    return "500 Internal Server Error";
  }

  return "";
}

/*
 * Convert a response structure into a complete HTTP response.
 *
 * The final result looks like:
 *
 *     HTTP/1.1 200 OK\r\n
 *     Content-Type: text/plain\r\n
 *     Content-Length: 5\r\n
 *     \r\n
 *     Hello
 */
int response_serialize(response_t *response, char **buffer) {
  int result = 0;

  ds_string_builder response_builder;
  ds_string_builder_init(&response_builder);

  /*
   * HTTP uses an empty line between headers and body.
   */
  ds_string_builder_append(&response_builder, "%s %s\r\n%s\r\n%s",
                           protocol_kind_serialize(response->protocol),
                           status_code_serialize(response->status_code),
                           headers_serialize(&response->headers),
                           response->content ? response->content : "");

  if (ds_string_builder_build(&response_builder, buffer) != 0) {
    return_defer(-1);
  }

  result = strlen(*buffer);

defer:
  return result;
}

/*
 * Serialize and send an HTTP response to the client.
 */
int response_write(int cfd, response_t *response) {
  int result = 0;

  char *buffer = NULL;

  int buffer_len = response_serialize(response, &buffer);

  if (buffer_len < 0) {
    return -1;
  }

  /*
   * write() may write fewer bytes than requested.
   *
   * Therefore we keep writing until the complete response
   * has been sent.
   */
  int total_written = 0;

  while (total_written < buffer_len) {
    int written =
        write(cfd, buffer + total_written, buffer_len - total_written);

    if (written <= 0) {
      return -1;
    }

    total_written += written;
  }

  result = total_written;

defer:
  return result;
}

/*
 * Convert request enum into text.
 */
const char *serialize_request_kind(request_kind kind) {
  switch (kind) {
  case GET:
    return "GET";

  case POST:
    return "POST";

  case PUT:
    return "PUT";

  case DELETE:
    return "DELETE";
  }

  return "";
}

/*
 * Remove '\r' from the end of a string.
 *
 * HTTP normally uses:
 *
 *     \r\n
 *
 * while the original code was mostly working with '\n'.
 */
void strip_carriage_return(char *str) {
  if (str == NULL) {
    return;
  }

  size_t len = strlen(str);

  if (len > 0 && str[len - 1] == '\r') {
    str[len - 1] = '\0';
  }
}

/*
 * Find Content-Length in an HTTP request.
 *
 * Example:
 *
 *     Content-Length: 12
 *
 * returns:
 *
 *     12
 *
 * If Content-Length doesn't exist, returns 0.
 */
int get_content_length(char *buffer) {
  char *content_length = strstr(buffer, "Content-Length:");

  if (content_length == NULL) {
    return 0;
  }

  content_length += strlen("Content-Length:");

  /*
   * Skip spaces.
   */
  while (*content_length == ' ' || *content_length == '\t') {
    content_length++;
  }

  return atoi(content_length);
}

/*
 * Parse an HTTP request.
 *
 * We support:
 *
 *     GET
 *     POST
 *     PUT
 *     DELETE
 *
 * Example request:
 *
 *     POST /hello.txt HTTP/1.1\r\n
 *     Content-Length: 5\r\n
 *     \r\n
 *     Hello
 */
int request_parse(char *buffer, unsigned int buffer_len, request_t *request) {
  int result = 0;

  char *line_end;
  char *headers_end;

  /*
   * Find the end of the first HTTP line.
   */
  line_end = strstr(buffer, "\n");

  if (line_end == NULL) {
    DS_LOG_ERROR("expected HTTP request line");
    return_defer(-1);
  }

  /*
   * Temporarily terminate the request line.
   */
  *line_end = '\0';

  /*
   * The request line has:
   *
   *     METHOD PATH PROTOCOL
   */
  char *verb = strtok(buffer, " ");
  char *path = strtok(NULL, " ");
  char *protocol = strtok(NULL, " ");

  if (verb == NULL) {
    DS_LOG_ERROR("expected HTTP verb");
    return_defer(-1);
  }

  if (path == NULL) {
    DS_LOG_ERROR("expected HTTP path");
    return_defer(-1);
  }

  if (protocol == NULL) {
    DS_LOG_ERROR("expected HTTP protocol");
    return_defer(-1);
  }

  /*
   * Remove '\r' if the request used CRLF.
   */
  strip_carriage_return(protocol);

  /*
   * Convert the method string into our enum.
   */
  if (strcmp(verb, "GET") == 0) {
    request->kind = GET;
  } else if (strcmp(verb, "POST") == 0) {
    request->kind = POST;
  } else if (strcmp(verb, "PUT") == 0) {
    request->kind = PUT;
  } else if (strcmp(verb, "DELETE") == 0) {
    request->kind = DELETE;
  } else {
    DS_LOG_ERROR("unsupported HTTP method: %s", verb);
    return_defer(-1);
  }

  /*
   * Make copies of path and protocol because the original
   * request buffer can be modified later.
   */
  request->path = strdup(path);

  if (request->path == NULL) {
    DS_LOG_ERROR("out of memory");
    return_defer(-1);
  }

  request->protocol = strdup(protocol);

  if (request->protocol == NULL) {
    free(request->path);
    request->path = NULL;

    DS_LOG_ERROR("out of memory");
    return_defer(-1);
  }

  /*
   * Find the empty line separating headers from body.
   *
   * HTTP:
   *
   *     headers...
   *     \r\n
   *     \r\n
   *     body
   *
   */
  headers_end = strstr(line_end + 1, "\r\n\r\n");

  if (headers_end != NULL) {
    /*
     * Body starts after "\r\n\r\n".
     */
    request->body = headers_end + 4;
  } else {
    /*
     * Also accept "\n\n" for simple testing.
     */
    headers_end = strstr(line_end + 1, "\n\n");

    if (headers_end != NULL) {
      request->body = headers_end + 2;
    } else {
      request->body = NULL;
    }
  }

  /*
   * Determine body length from Content-Length.
   */
  request->body_len = get_content_length(line_end + 1);

  /*
   * Never allow the body length to go outside the bytes that
   * were actually received.
   */
  if (request->body != NULL) {
    unsigned long body_offset = (unsigned long)(request->body - buffer);

    if (body_offset >= buffer_len) {
      request->body = NULL;
      request->body_len = 0;
    } else {
      unsigned int available = buffer_len - body_offset;

      if ((unsigned int)request->body_len > available) {
        request->body_len = available;
      }
    }
  } else {
    request->body_len = 0;
  }

  DS_LOG_DEBUG("method=%s path=%s protocol=%s body_len=%d",
               serialize_request_kind(request->kind), request->path,
               request->protocol, request->body_len);

defer:
  return result;
}

/*
 * Build the full filesystem path.
 *
 * Example:
 *
 *     prefix = /home/user/site
 *     path   = /index.html
 *
 * result:
 *
 *     /home/user/site/index.html
 */
int build_full_path(char *prefix, char *path, char **full_path) {
  ds_string_builder path_builder;

  ds_string_builder_init(&path_builder);

  /*
   * path + 1 removes the leading '/'.
   */
  if (ds_string_builder_append(&path_builder, "%s/%s", prefix,
                               path[0] == '/' ? path + 1 : path) != 0) {

    DS_LOG_ERROR("could not create path string");

    return -1;
  }

  if (ds_string_builder_build(&path_builder, full_path) != 0) {

    DS_LOG_ERROR("could not build full path");

    return -1;
  }

  return 0;
}

/*
 * Read a file or generate a directory listing.
 *
 * Used by GET.
 */
int read_path(char *prefix, char *path, char **content) {
  int result = 0;

  char *full_path = NULL;

  if (build_full_path(prefix, path, &full_path) != 0) {

    return_defer(-1);
  }

  DS_LOG_DEBUG("full path to the file/directory is %s", full_path);

  struct stat path_stat;

  if (stat(full_path, &path_stat) != 0) {
    DS_LOG_ERROR("stat: %s", strerror(errno));

    return_defer(-1);
  }

  int content_len = 0;

  /*
   * Regular file.
   */
  if (S_ISREG(path_stat.st_mode)) {

    content_len = ds_io_read_file(full_path, content);

    if (content_len < 0) {
      return_defer(-1);
    }

    /*
     * Directory.
     */
  } else if (S_ISDIR(path_stat.st_mode)) {

    ds_string_builder directory_builder;

    ds_string_builder_init(&directory_builder);

    if (ds_string_builder_append(&directory_builder,
                                 "<!DOCTYPE HTML>\n"
                                 "<html lang=\"en\">\n"
                                 "<head>\n"
                                 "<meta charset=\"utf-8\">\n"
                                 "<title>Directory listing for %s</title>\n"
                                 "</head>\n"
                                 "<body>\n"
                                 "<h1>Directory listing for %s</h1>\n"
                                 "<hr>\n"
                                 "<ul>\n",
                                 path, path) != 0) {

      DS_LOG_ERROR("could not append to response string");

      return_defer(-1);
    }

    DIR *directory = opendir(full_path);

    if (directory == NULL) {
      DS_LOG_ERROR("opendir: %s", strerror(errno));

      return_defer(-1);
    }

    struct dirent *dir;

    while ((dir = readdir(directory)) != NULL) {

      /*
       * Build a correct URL for the directory entry.
       *
       * For the root directory:
       *     path = "/"
       *     "/%s" gives "/main"
       *
       * For another directory:
       *     path = "/folder"
       *     "%s/%s" gives "/folder/main"
       *
       * This avoids generating "//main" for the root directory.
       */
      int append_result;

      if (strcmp(path, "/") == 0) {
        append_result = ds_string_builder_append(
            &directory_builder, "<li><a href=\"/%s\">%s</a></li>\n",
            dir->d_name, dir->d_name);
      } else {
        append_result = ds_string_builder_append(
            &directory_builder, "<li><a href=\"%s/%s\">%s</a></li>\n", path,
            dir->d_name, dir->d_name);
      }

      if (append_result != 0) {
        DS_LOG_ERROR("could not append directory entry");
        continue;
      }
    }

    if (closedir(directory) != 0) {
      DS_LOG_ERROR("closedir: %s", strerror(errno));

      return_defer(-1);
    }

    /*
     * Small browser-based HTTP API tester.
     *
     * The JavaScript uses fetch() to call this same server with
     * GET, POST, PUT, and DELETE, so the server can be tested
     * directly from the browser without curl.
     */
    ds_string_builder_append(
        &directory_builder,
        "</ul>\n"
        "<hr>\n"
        "<h2>HTTP API Tester</h2>\n"
        "<p>Use this panel to test the server from your browser.</p>\n"
        "<div style=\"max-width:700px;font-family:Arial,sans-serif;\">\n"
        "  <label for=\"method\">Method:</label>\n"
        "  <select id=\"method\">\n"
        "    <option>GET</option>\n"
        "    <option>POST</option>\n"
        "    <option>PUT</option>\n"
        "    <option>DELETE</option>\n"
        "  </select>\n"
        "  <label for=\"path\">Path:</label>\n"
        "  <input id=\"path\" value=\"/test.txt\" style=\"width:220px\">\n"
        "  <br><br>\n"
        "  <label for=\"body\">Body:</label><br>\n"
        "  <textarea id=\"body\" rows=\"4\" cols=\"60\">Hello from "
        "browser</textarea>\n"
        "  <br>\n"
        "  <button type=\"button\" onclick=\"sendRequest()\">Send "
        "Request</button>\n"
        "  <button type=\"button\" "
        "onclick=\"document.getElementById('response').textContent=''\"\n"
        "          >Clear</button>\n"
        "  <h3>Response</h3>\n"
        "  <pre id=\"response\" style=\"white-space:pre-wrap;border:1px solid "
        "#aaa;padding:10px;min-height:80px;\"></pre>\n"
        "</div>\n"
        "<script>\n"
        "async function sendRequest() {\n"
        "  const method = document.getElementById('method').value;\n"
        "  const path = document.getElementById('path').value.trim();\n"
        "  const body = document.getElementById('body').value;\n"
        "  const output = document.getElementById('response');\n"
        "\n"
        "  if (!path.startsWith('/')) {\n"
        "    output.textContent = 'Path must start with /';\n"
        "    return;\n"
        "  }\n"
        "\n"
        "  output.textContent = 'Sending request...';\n"
        "\n"
        "  try {\n"
        "    const options = { method: method };\n"
        "\n"
        "    if (method === 'POST' || method === 'PUT') {\n"
        "      options.headers = { 'Content-Type': 'text/plain' };\n"
        "      options.body = body;\n"
        "    }\n"
        "\n"
        "    const response = await fetch(path, options);\n"
        "    const text = await response.text();\n"
        "\n"
        "    output.textContent =\n"
        "      'HTTP ' + response.status + ' ' + response.statusText + "
        "'\\n\\n' +\n"
        "      text;\n"
        "  } catch (error) {\n"
        "    output.textContent = 'Request failed: ' + error;\n"
        "  }\n"
        "}\n"
        "</script>\n"
        "<hr>\n"
        "</body>\n"
        "</html>\n");

    if (ds_string_builder_build(&directory_builder, content) != 0) {

      DS_LOG_ERROR("could not build directory response");

      return_defer(-1);
    }

    content_len = strlen(*content);

  } else {

    DS_LOG_ERROR("filesystem object type not supported");

    return_defer(-1);
  }

  result = content_len;

defer:
  return result;
}

/*
 * Add a header to a response.
 */
int headers_append_value(ds_dynamic_array *headers, char *key, char *value) {
  header_t header = {.key = key, .value = value};

  return ds_dynamic_array_append(headers, &header);
}

/*
 * Convert integer to dynamically allocated string.
 *
 * Example:
 *
 *     itoa(123)
 *
 * gives:
 *
 *     "123"
 */
char *itoa(int value) {
  ds_string_builder string_builder;

  ds_string_builder_init(&string_builder);

  ds_string_builder_append(&string_builder, "%d", value);

  char *buffer = NULL;

  if (ds_string_builder_build(&string_builder, &buffer) != 0) {

    return NULL;
  }

  return buffer;
}

/*
 * Determine the Content-Type from the file extension.
 */
char *get_content_type(char *path) {
  /*
   * Directory URLs end with '/'.
   * Our server generates an HTML directory listing for them,
   * so the correct MIME type is text/html.
   */
  size_t path_len = strlen(path);

  if (path_len > 0 && path[path_len - 1] == '/') {
    return "text/html";
  }

  char *last_dot = strrchr(path, '.');

  if (last_dot == NULL) {
    return "text/plain";
  }

  char *suffix = last_dot + 1;

  if (strcmp(suffix, "html") == 0 || strcmp(suffix, "htm") == 0) {
    return "text/html";
  } else if (strcmp(suffix, "css") == 0) {
    return "text/css";
  } else if (strcmp(suffix, "js") == 0) {
    return "application/javascript";
  } else if (strcmp(suffix, "json") == 0) {
    return "application/json";
  } else if (strcmp(suffix, "pdf") == 0) {
    return "application/pdf";
  } else if (strcmp(suffix, "txt") == 0) {
    return "text/plain";
  } else if (strcmp(suffix, "png") == 0) {
    return "image/png";
  } else if (strcmp(suffix, "jpg") == 0 || strcmp(suffix, "jpeg") == 0) {
    return "image/jpeg";
  }

  return "application/octet-stream";
}

/*
 * Write exactly body_len bytes to a file.
 *
 * This function is binary-safe.
 */
int write_file_content(char *path, char *body, int body_len, int flags) {
  /*
   * 0666 means:
   *
   *     owner: read/write
   *     group: read/write
   *     others: read/write
   *
   * The final permissions are still affected by umask.
   */
  int fd = open(path, flags, 0666);

  if (fd == -1) {
    DS_LOG_ERROR("open: %s", strerror(errno));

    return -1;
  }

  int total_written = 0;

  while (total_written < body_len) {

    int written = write(fd, body + total_written, body_len - total_written);

    if (written <= 0) {
      DS_LOG_ERROR("write: %s", strerror(errno));

      close(fd);

      return -1;
    }

    total_written += written;
  }

  if (close(fd) == -1) {
    DS_LOG_ERROR("close: %s", strerror(errno));

    return -1;
  }

  return total_written;
}

/*
 * Handle POST.
 *
 * For this educational file server:
 *
 *     POST /file.txt
 *
 * creates a new file.
 *
 * If the file already exists:
 *
 *     409 Conflict
 */
int handle_post(char *prefix_directory, request_t *request) {
  char *full_path = NULL;

  if (build_full_path(prefix_directory, request->path, &full_path) != 0) {

    return -1;
  }

  /*
   * O_CREAT  -> create if it doesn't exist
   * O_EXCL   -> fail if it already exists
   * O_WRONLY -> open for writing
   */
  int result =
      write_file_content(full_path, request->body ? request->body : "",
                         request->body_len, O_WRONLY | O_CREAT | O_EXCL);

  return result < 0 ? -1 : 0;
}

/*
 * Handle PUT.
 *
 * PUT replaces the complete content of a file.
 *
 * If the file doesn't exist, it is created.
 */
int handle_put(char *prefix_directory, request_t *request) {
  char *full_path = NULL;

  if (build_full_path(prefix_directory, request->path, &full_path) != 0) {

    return -1;
  }

  /*
   * O_TRUNC means:
   *
   *     if the file exists,
   *     remove its old content.
   *
   * O_CREAT means:
   *
   *     create it if necessary.
   */
  int result =
      write_file_content(full_path, request->body ? request->body : "",
                         request->body_len, O_WRONLY | O_CREAT | O_TRUNC);

  return result < 0 ? -1 : 0;
}

/*
 * Handle DELETE.
 *
 * For this simple server we delete regular files.
 *
 * Directories are not deleted here.
 */
int handle_delete(char *prefix_directory, request_t *request) {
  char *full_path = NULL;

  if (build_full_path(prefix_directory, request->path, &full_path) != 0) {

    return -1;
  }

  struct stat path_stat;

  if (stat(full_path, &path_stat) != 0) {
    return -1;
  }

  /*
   * Only delete regular files.
   */
  if (!S_ISREG(path_stat.st_mode)) {
    errno = EISDIR;
    return -1;
  }

  if (unlink(full_path) != 0) {
    DS_LOG_ERROR("unlink: %s", strerror(errno));

    return -1;
  }

  return 0;
}

/*
 * Main request handler.
 *
 * Flow:
 *
 *     read()
 *       |
 *       v
 *     request_parse()
 *       |
 *       +---- GET ------> read_path()
 *       |
 *       +---- POST -----> handle_post()
 *       |
 *       +---- PUT ------> handle_put()
 *       |
 *       +---- DELETE ---> handle_delete()
 *       |
 *       v
 *     build response
 *       |
 *       v
 *     write()
 */
int handle_request(int cfd, char *prefix_directory) {
  int result = 0;

  int content_len;

  unsigned int buffer_len = 0;

  char buffer[MAX_LEN] = {0};

  request_t request = {0};

  char *content = NULL;

  response_t response = {0};

  /*
   * Prepare response headers array.
   */
  ds_dynamic_array_init(&response.headers, sizeof(header_t));

  response.protocol = HTTP_1_1;

  /*
   * Read the HTTP request from the TCP socket.
   */
  result = read(cfd, buffer, MAX_LEN - 1);

  if (result == -1) {

    response.status_code = 500;

    headers_append_value(&response.headers, "Content-Type", "text/plain");

    headers_append_value(&response.headers, "Content-Length",
                         itoa(strlen(INTERNAL_ERROR_STR)));

    response.content = INTERNAL_ERROR_STR;

    DS_LOG_ERROR("read: %s", strerror(errno));

    goto defer;
  }

  /*
   * Always terminate the received bytes as a C string.
   */
  buffer[result] = '\0';

  buffer_len = result;

  /*
   * Parse the HTTP request.
   */
  if (request_parse(buffer, buffer_len, &request) == -1) {

    DS_LOG_ERROR("request parse");

    response.status_code = 400;

    headers_append_value(&response.headers, "Content-Type", "text/plain");

    headers_append_value(&response.headers, "Content-Length",
                         itoa(strlen(BAD_REQUEST_STR)));

    response.content = BAD_REQUEST_STR;

    goto defer;
  }

  DS_LOG_INFO("incoming request %s %s", serialize_request_kind(request.kind),
              request.path);

  /*
   * =========================================================
   * GET
   * =========================================================
   *
   * GET reads an existing file or generates a directory
   * listing.
   */
  if (request.kind == GET) {

    result = read_path(prefix_directory, request.path, &content);

    if (result == -1) {

      DS_LOG_ERROR("read path");

      response.status_code = 404;

      headers_append_value(&response.headers, "Content-Type", "text/plain");

      headers_append_value(&response.headers, "Content-Length",
                           itoa(strlen(NOT_FOUND_STR)));

      response.content = NOT_FOUND_STR;

      goto defer;
    }

    content_len = result;

    response.status_code = 200;

    headers_append_value(&response.headers, "Content-Type",
                         get_content_type(request.path));

    headers_append_value(&response.headers, "Content-Length",
                         itoa(content_len));

    response.content = content;
  }

  /*
   * =========================================================
   * POST
   * =========================================================
   *
   * POST creates a NEW file.
   *
   * Example:
   *
   *     POST /hello.txt
   *
   *     Hello
   *
   * creates:
   *
   *     hello.txt
   *
   * containing:
   *
   *     Hello
   */
  else if (request.kind == POST) {

    char *full_path = NULL;

    build_full_path(prefix_directory, request.path, &full_path);

    /*
     * If the target already exists,
     * POST returns 409 Conflict.
     */
    struct stat path_stat;

    if (stat(full_path, &path_stat) == 0) {

      response.status_code = 409;

      headers_append_value(&response.headers, "Content-Type", "text/plain");

      headers_append_value(&response.headers, "Content-Length",
                           itoa(strlen("File already exists")));

      response.content = "File already exists";

      goto defer;
    }

    if (handle_post(prefix_directory, &request) == -1) {

      response.status_code = 500;

      headers_append_value(&response.headers, "Content-Type", "text/plain");

      headers_append_value(&response.headers, "Content-Length",
                           itoa(strlen(INTERNAL_ERROR_STR)));

      response.content = INTERNAL_ERROR_STR;

      goto defer;
    }

    response.status_code = 201;

    headers_append_value(&response.headers, "Content-Type", "text/plain");

    headers_append_value(&response.headers, "Content-Length",
                         itoa(strlen(CREATED_STR)));

    response.content = CREATED_STR;
  }

  /*
   * =========================================================
   * PUT
   * =========================================================
   *
   * PUT creates or replaces the complete content of a file.
   */
  else if (request.kind == PUT) {

    if (handle_put(prefix_directory, &request) == -1) {

      response.status_code = 500;

      headers_append_value(&response.headers, "Content-Type", "text/plain");

      headers_append_value(&response.headers, "Content-Length",
                           itoa(strlen(INTERNAL_ERROR_STR)));

      response.content = INTERNAL_ERROR_STR;

      goto defer;
    }

    response.status_code = 200;

    headers_append_value(&response.headers, "Content-Type", "text/plain");

    headers_append_value(&response.headers, "Content-Length",
                         itoa(strlen("Updated")));

    response.content = "Updated";
  }

  /*
   * =========================================================
   * DELETE
   * =========================================================
   *
   * DELETE removes a file.
   */
  else if (request.kind == DELETE) {

    char *full_path = NULL;

    build_full_path(prefix_directory, request.path, &full_path);

    /*
     * Check if the target exists.
     */
    struct stat path_stat;

    if (stat(full_path, &path_stat) != 0) {

      response.status_code = 404;

      headers_append_value(&response.headers, "Content-Type", "text/plain");

      headers_append_value(&response.headers, "Content-Length",
                           itoa(strlen(NOT_FOUND_STR)));

      response.content = NOT_FOUND_STR;

      goto defer;
    }

    if (handle_delete(prefix_directory, &request) == -1) {

      response.status_code = 500;

      headers_append_value(&response.headers, "Content-Type", "text/plain");

      headers_append_value(&response.headers, "Content-Length",
                           itoa(strlen(INTERNAL_ERROR_STR)));

      response.content = INTERNAL_ERROR_STR;

      goto defer;
    }

    response.status_code = 200;

    headers_append_value(&response.headers, "Content-Type", "text/plain");

    headers_append_value(&response.headers, "Content-Length",
                         itoa(strlen(DELETED_STR)));

    response.content = DELETED_STR;
  }

  /*
   * This should normally never happen because request_parse()
   * rejects unsupported methods.
   */
  else {

    response.status_code = 405;

    headers_append_value(&response.headers, "Content-Type", "text/plain");

    headers_append_value(&response.headers, "Content-Length",
                         itoa(strlen(METHOD_NOT_ALLOWED_STR)));

    response.content = METHOD_NOT_ALLOWED_STR;
  }

defer:

  /*
   * Send the response to the client.
   */
  response_write(cfd, &response);

  /*
   * Close this client's connection.
   *
   * The original code was missing this.
   */
  close(cfd);

  /*
   * Free memory allocated by request_parse().
   */
  if (request.path != NULL) {
    free(request.path);
  }

  if (request.protocol != NULL) {
    free(request.protocol);
  }

  return result;
}

/*
 * =============================================================
 * MAIN
 * =============================================================
 *
 * Starts the TCP server.
 */
int main(int argc, char *argv[]) {
  int sfd;
  int cfd;
  int result;
  int port;

  char *prefix_directory = NULL;

  struct sockaddr_in server_addr;

  ds_argparse_parser parser;

  /*
   * =========================================================
   * Determine current working directory.
   * =========================================================
   */
  char cwd[PATH_MAX];

  if (getcwd(cwd, sizeof(cwd)) == NULL) {

    DS_PANIC("getcwd: %s", strerror(errno));
  }

  /*
   * Start building the directory that will be served.
   */
  ds_string_builder directory_builder;

  ds_string_builder_init(&directory_builder);

  ds_string_builder_append(&directory_builder, "%s", cwd);

  /*
   * =========================================================
   * Command-line argument parser
   * =========================================================
   */
  ds_argparse_parser_init(&parser, "http.server", "A clone of http.server in C",
                          "1.0");

  /*
   * -p / --port
   *
   * Example:
   *
   *     ./http.server -p 8080
   */
  ds_argparse_add_argument(
      &parser,
      (ds_argparse_options){.short_name = 'p',
                            .long_name = "port",
                            .description = "bind to this port (default: 8000)",
                            .type = ARGUMENT_TYPE_POSITIONAL,
                            .required = 0});

  /*
   * -d / --directory
   *
   * Example:
   *
   *     ./http.server -d website
   */
  ds_argparse_add_argument(
      &parser,
      (ds_argparse_options){
          .short_name = 'd',
          .long_name = "directory",
          .description = "serve this directory (default: current directory)",
          .type = ARGUMENT_TYPE_VALUE,
          .required = 0});

  /*
   * Parse command-line arguments.
   */
  ds_argparse_parse(&parser, argc, argv);

  /*
   * Get port.
   *
   * Default:
   *
   *     8000
   */
  char *port_value = ds_argparse_get_value(&parser, "port");

  port = (port_value == NULL) ? 8000 : atoi(port_value);

  /*
   * Get directory.
   */
  char *directory_value = ds_argparse_get_value(&parser, "directory");

  if (directory_value != NULL) {

    ds_string_builder_append(&directory_builder, "/%s", directory_value);
  }

  /*
   * Build final directory path.
   */
  if (ds_string_builder_build(&directory_builder, &prefix_directory) != 0) {

    DS_PANIC("could not build directory path");
  }

  /*
   * =========================================================
   * Create TCP socket
   * =========================================================
   *
   * AF_INET:
   *     IPv4
   *
   * SOCK_STREAM:
   *     TCP
   */
  sfd = socket(AF_INET, SOCK_STREAM, 0);

  if (sfd == -1) {

    DS_PANIC("socket: %s", strerror(errno));
  }

  /*
   * =========================================================
   * Configure server address
   * =========================================================
   */
  server_addr.sin_family = AF_INET;

  /*
   * Convert port to network byte order.
   */
  server_addr.sin_port = htons(port);

  /*
   * 0.0.0.0 means:
   *
   *     listen on all IPv4 interfaces.
   */
  inet_pton(AF_INET, "0.0.0.0", &server_addr.sin_addr);

  /*
   * =========================================================
   * Bind socket to IP + port
   * =========================================================
   */
  if (bind(sfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {

    DS_PANIC("bind: %s", strerror(errno));
  }

  /*
   * =========================================================
   * Start listening
   * =========================================================
   */
  if (listen(sfd, MAX_LISTEN) == -1) {

    DS_PANIC("listen: %s", strerror(errno));
  }

  DS_LOG_INFO("listening on port %d serving from %s", port, prefix_directory);

  /*
   * =========================================================
   * Accept clients forever
   * =========================================================
   */
  while (1) {

    struct sockaddr_in client_addr;

    socklen_t client_addr_size = sizeof(client_addr);

    /*
     * Wait for a client.
     */
    cfd = accept(sfd, (struct sockaddr *)&client_addr, &client_addr_size);

    if (cfd == -1) {

      DS_LOG_ERROR("accept: %s", strerror(errno));

      continue;
    }

    /*
     * Process this HTTP request.
     */
    handle_request(cfd, prefix_directory);
  }

  /*
   * We normally never reach this because of while(1).
   */
  result = close(sfd);

  if (result == -1) {

    DS_PANIC("close");
  }

  return 0;
}
