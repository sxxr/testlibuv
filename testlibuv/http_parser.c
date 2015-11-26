#include "http_parser.h"


int http_parse(http_ctx *parser, uint8_t *data, size_t size) {

	char *cur_attr = parser->curattr;
	size_t curr_attr_len = parser->curattrlen;
	char *cur_val = parser->curval;
	char *cur_val_len = parser->curvallen;

	size_t remain = size;

	int status = parser->status;

	int err = http_ok;

	char *p = (char*)parser->next;
	while (remain > 0
		&& err == http_ok) {

		switch (status)
		{
		case ps_init: // 开始解析method
			parser->curattr = p;
			parser->curattrlen = 0;

			// http 协议由三部分组成: 请求行，消息包头，请求正文
			// 请求行包括：method uri version
			status = ps_method;
			parser->method = p;
			parser->methodlen = 0;
			// break;
		case ps_method:
			if (p[0] == ' ') {

				if (parser->methodlen == 0) {

					err = http_bad_method;
					break;
				}

				// 开始解析:请求行-uri
				status = ps_uri;
				parser->uri = p+1;
				parser->urilen = 0;
				break;
			}

			parser->methodlen++;
			break;
		case ps_uri:
			if (p[0] == ' ') {

				if (parser->urilen == 0) {

					err = http_bad_uri;
					break;
				}

				// 直接根据uri执行命令
				// 开始解析:请求行-version
				// status = ps_version;
				err = http_exec_cmd;

				parser->ver = p+1;
				parser->verlen = 0;
				break;
			}

			parser->urilen++;
			break;
		case ps_attr: // 解析attr
			if (p[0] == '\r' || p[0] == '\n')
			{
				// 说明需要重新计算attr，及val
				status = ps_init;

				parser->curattr = p;
				parser->curattrlen = 0;
				break;
			}
			if (p[0] == ' ' || p[0] == '\t')
			{
				break;
			}
			if (p[0] == ':')
			{
				// 说明开始转入解析val
				status = ps_value;

				parser->curval = p + 1;
				parser->curvallen = 0;
				break;
			}
			parser->curattrlen++;
			break;
		case ps_value: // 解析val;

			if (p[0] == '\r' || p[0] == '\n')
			{
				// 说明value解析完毕，
				// 解析字段
				if (memcpy(parser->curattr, "URI", parser->curattrlen)) {

					parser->uri = parser->curval;
					parser->urilen = parser->curvallen;

					err = http_exec_cmd;
				}

				// 重新计算attr，及val
				status = ps_init;
				parser->curattr = p;
				parser->curattrlen = 0;
				break;
			}
			parser->curvallen++;
		}

		p++;
		remain--;
	}

	parser->status = status;
	parser->next = p;
	parser->remain = remain;

	return err;
}

