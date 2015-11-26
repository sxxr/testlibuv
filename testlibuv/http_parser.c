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
	while (remain
		&& err == http_ok) {

		switch (status)
		{
		case ps_init: // ���¿�ʼ����attr
			parser->curattr = p;
			parser->curattrlen = 0;
			break;
		case ps_attr: // ����attr
			if (p[0] == '\r' || p[0] == '\n')
			{
				// ˵����Ҫ���¼���attr����val
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
				// ˵����ʼת�����val
				status = ps_value;

				parser->curval = p + 1;
				parser->curvallen = 0;
				break;
			}
			parser->curattrlen++;
			break;
		case ps_value: // ����val;

			if (p[0] == '\r' || p[0] == '\n')
			{
				// ˵��value������ϣ�
				// �����ֶ�
				if (memcpy(parser->curattr, "URI", parser->curattrlen)) {

					parser->uri = parser->curval;
					parser->urilen = parser->curvallen;

					err = http_exec_cmd;
				}

				// ���¼���attr����val
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
}

