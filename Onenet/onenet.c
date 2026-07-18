#include "stm32f10x.h"

#include "esp8266.h"

#include "onenet.h"
#include "mqttkit.h"

#include "bsp_usart.h"
#include "bsp_delay.h"

#include <string.h>
#include <stdio.h>

#include "cJSON.h"

#define PROID		"53BV12EYcY"

#define TOKEN	"version=2018-10-31&res=products%2F53BV12EYcY%2Fdevices%2Fpark1&et=1815919855&method=md5&sign=WSMSUPJ7K3%2B%2ByxiohKCfFQ%3D%3D"

#define DEVID		"park1"

const char PropertySetReplyTopic[] = "$sys/53BV12EYcY/park1/thing/property/set_reply";

const char PropertyPostTopic[] = "$sys/53BV12EYcY/park1/thing/property/post";

_Bool OneNet_DevLink(void)
{
	MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

	unsigned char *dataPtr;
	_Bool status = 1;

	Usart_Printf(USART_DEBUG, "OneNet_DevLink\r\n"
							"PROID: %s,	TOKEN: %s, DEVID:%s\r\n"
                        , PROID, TOKEN, DEVID);

	if(MQTT_PacketConnect(PROID, TOKEN, DEVID, 256, 1, MQTT_QOS_LEVEL0, NULL, NULL, 0, &mqttPacket) == 0)
	{
		ESP8266_SendData(mqttPacket._data, mqttPacket._len);
		dataPtr = ESP8266_GetIPD(250);

		if(dataPtr != NULL)
		{
			if(MQTT_UnPacketRecv(dataPtr) == MQTT_PKT_CONNACK)
			{
				switch(MQTT_UnPacketConnectAck(dataPtr))
				{
					case 0:Usart_Printf(USART_DEBUG, "Tips:	连接成功\r\n");status = 0;break;

					case 1:Usart_Printf(USART_DEBUG, "WARN:	连接失败，协议错误\r\n");break;
					case 2:Usart_Printf(USART_DEBUG, "WARN:	连接失败，非法clientid\r\n");break;
					case 3:Usart_Printf(USART_DEBUG, "WARN:	连接失败，服务器失败\r\n");break;
					case 4:Usart_Printf(USART_DEBUG, "WARN:	连接失败，用户名或密码错误\r\n");break;
					case 5:Usart_Printf(USART_DEBUG, "WARN:	连接失败，非法授权(check token)\r\n");break;

					default:Usart_Printf(USART_DEBUG, "ERR:	连接失败，未知错误\r\n");break;
				}
			}
		}

		MQTT_DeleteBuffer(&mqttPacket);
	}
	else
		Usart_Printf(USART_DEBUG, "WARN:	MQTT_PacketConnect Failed\r\n");

	return status;
}

void OneNet_Subscribe(const char *topics[], unsigned char topic_cnt)
{
	unsigned char i = 0;

	MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

	for(; i < topic_cnt; i++)
		Usart_Printf(USART_DEBUG, "Subscribe Topic: %s\r\n", topics[i]);

	if(MQTT_PacketSubscribe(MQTT_SUBSCRIBE_ID, MQTT_QOS_LEVEL0, topics, topic_cnt, &mqttPacket) == 0)
	{
		ESP8266_SendData(mqttPacket._data, mqttPacket._len);

		MQTT_DeleteBuffer(&mqttPacket);
	}
}

void OneNet_Publish(const char *topic, const char *msg)
{
	MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

	Usart_Printf(USART_DEBUG, "Publish Topic: %s, Msg: %s\r\n", topic, msg);

	if(MQTT_PacketPublish(MQTT_PUBLISH_ID, topic, msg, strlen(msg),MQTT_QOS_LEVEL0, 0, 1, &mqttPacket) == 0)
	{
		ESP8266_SendData(mqttPacket._data, mqttPacket._len);

		MQTT_DeleteBuffer(&mqttPacket);
	}
}

void OneNet_SendPropertyReply(const char *id, int code, const char *msg)
{
	char replyBuf[128];
	
	sprintf(replyBuf, "{\"id\":\"%s\",\"code\":%d,\"msg\":\"%s\"}", id, code, msg);
	
	Usart_Printf(USART_DEBUG, "Property Reply: %s\r\n", replyBuf);
	
	OneNet_Publish(PropertySetReplyTopic, replyBuf);
}

void OneNet_RevPro(unsigned char *cmd)
{
	MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

	char *req_payload = NULL;
	char *cmdid_topic = NULL;

	unsigned short topic_len = 0;
	unsigned short req_len = 0;

	unsigned char type = 0;
	unsigned char qos = 0;
	static unsigned short pkt_id = 0;

	short result = 0;
	cJSON *json = NULL;
	cJSON *id_json = NULL;
	cJSON *params_json = NULL;
	
	char reqId[32] = "0";
	int respCode = 200;
	const char *respMsg = "success";

	type = MQTT_UnPacketRecv(cmd);
	switch(type)
	{
		case MQTT_PKT_CMD:

			result = MQTT_UnPacketCmd(cmd, &cmdid_topic, &req_payload, &req_len);
			if(result == 0)
			{
				Usart_Printf(USART_DEBUG, "cmdid: %s, req: %s, req_len: %d\r\n", cmdid_topic, req_payload, req_len);

				if(MQTT_PacketCmdResp(cmdid_topic, req_payload, &mqttPacket) == 0)
				{
					Usart_Printf(USART_DEBUG, "Tips:	发送CmdResp\r\n");

					ESP8266_SendData(mqttPacket._data, mqttPacket._len);
					MQTT_DeleteBuffer(&mqttPacket);
				}
			}

		break;

		case MQTT_PKT_PUBLISH:

			result = MQTT_UnPacketPublish(cmd, &cmdid_topic, &topic_len, &req_payload, &req_len, &qos, &pkt_id);
			if(result == 0)
			{
				Usart_Printf(USART_DEBUG, "topic: %s, payload: %s\r\n", cmdid_topic, req_payload);

				json = cJSON_Parse(req_payload);
				if (json == NULL)
				{
					Usart_Printf(USART_DEBUG, "JSON parse failed\r\n");
					respCode = 400;
					respMsg = "invalid json";
				}
				else
				{
					id_json = cJSON_GetObjectItem(json, "id");
					if (id_json != NULL && id_json->valuestring != NULL)
					{
						strncpy(reqId, id_json->valuestring, sizeof(reqId) - 1);
					}
					
					params_json = cJSON_GetObjectItem(json, "params");
					if (params_json == NULL)
					{
						Usart_Printf(USART_DEBUG, "No params found\r\n");
						respCode = 400;
						respMsg = "no params";
					}
					else
					{
						
						//这里放驱动代码
					}
				}
				
				OneNet_SendPropertyReply(reqId, respCode, respMsg);
			}
		break;

		case MQTT_PKT_PUBACK:

			if(MQTT_UnPacketPublishAck(cmd) == 0)
				Usart_Printf(USART_DEBUG, "Tips:	MQTT Publish Send OK\r\n");

		break;

		case MQTT_PKT_PUBREC:

			if(MQTT_UnPacketPublishRec(cmd) == 0)
			{
				Usart_Printf(USART_DEBUG, "Tips:	Rev PublishRec\r\n");
				if(MQTT_PacketPublishRel(MQTT_PUBLISH_ID, &mqttPacket) == 0)
				{
					Usart_Printf(USART_DEBUG, "Tips:	Send PublishRel\r\n");
					ESP8266_SendData(mqttPacket._data, mqttPacket._len);
					MQTT_DeleteBuffer(&mqttPacket);
				}
			}

		break;

		case MQTT_PKT_PUBREL:

			if(MQTT_UnPacketPublishRel(cmd, pkt_id) == 0)
			{
				Usart_Printf(USART_DEBUG, "Tips:	Rev PublishRel\r\n");
				if(MQTT_PacketPublishComp(MQTT_PUBLISH_ID, &mqttPacket) == 0)
				{
					Usart_Printf(USART_DEBUG, "Tips:	Send PublishComp\r\n");
					ESP8266_SendData(mqttPacket._data, mqttPacket._len);
					MQTT_DeleteBuffer(&mqttPacket);
				}
			}

		break;

		case MQTT_PKT_PUBCOMP:

			if(MQTT_UnPacketPublishComp(cmd) == 0)
			{
				Usart_Printf(USART_DEBUG, "Tips:	Rev PublishComp\r\n");
			}

		break;

		case MQTT_PKT_SUBACK:

			if(MQTT_UnPacketSubscribe(cmd) == 0)
				Usart_Printf(USART_DEBUG, "Tips:	MQTT Subscribe OK\r\n");
			else
				Usart_Printf(USART_DEBUG, "Tips:	MQTT Subscribe Err\r\n");

		break;

		case MQTT_PKT_UNSUBACK:

			if(MQTT_UnPacketUnSubscribe(cmd) == 0)
				Usart_Printf(USART_DEBUG, "Tips:	MQTT UnSubscribe OK\r\n");
			else
				Usart_Printf(USART_DEBUG, "Tips:	MQTT UnSubscribe Err\r\n");

		break;

		default:
			result = -1;
		break;
	}

	ESP8266_Clear();

	if(result == -1)
		return;

	if(type == MQTT_PKT_CMD || type == MQTT_PKT_PUBLISH)
	{
		MQTT_FreeBuffer(cmdid_topic);
		MQTT_FreeBuffer(req_payload);
	}
}