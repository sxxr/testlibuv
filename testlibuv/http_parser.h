
#ifndef HTTP_PARSER_H_
#define HTTP_PARSER_H_

#include <stddef.h>
#include <stdint.h>

#define HTTP_ERR_MAP(V)                                                         \
  V(-1, bad_version, "Bad protocol version.")                                 \
  V(-2, bad_cmd, "Bad protocol command.")                                     \
  V(-3, bad_atyp, "Bad address type.")                                        \
  V(-4, bad_method, "Bad http method.")                                        \
  V(-5, bad_uri, "Bad http uri.")                                        \
  V(0, ok, "No error.")                                                       \
  V(1, exec_cmd, "Execute command.")											\


typedef enum {
#define HTTP_ERR_GEN(code, name, _) http_ ## name = code,
	HTTP_ERR_MAP(HTTP_ERR_GEN)
#undef HTTP_ERR_GEN
	http_max_errors
} http_err;


typedef enum {
	ps_init,
	ps_method,
	ps_uri,
	ps_version, 
	ps_attr,
	ps_value,
}parse_status;


/* define for http header
*/
typedef struct {
	char *method;
	int methodlen;

	char *uri;
	int urilen;

	char *ver;
	int verlen;


	/* for parse*/
	char *curattr;
	size_t curattrlen;
	char *curval;
	size_t curvallen;
	int status;

	char *next;
	size_t remain;
}http_ctx;

int http_parse(http_ctx *parser, uint8_t *data, size_t size);

#endif // HTTP_PARSER_H_