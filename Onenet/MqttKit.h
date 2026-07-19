/****************************************************************************
 * MQTT协议工具库头文件 - MqttKit.h
 * 
 * 功能描述:
 *   提供MQTT协议报文的构建和解析功能
 *   支持MQTT v3.1.1协议规范
 *   支持QOS 0/1/2三种服务质量等级
 * 
 * 功能特性:
 *   - 连接/断开连接
 *   - 发布/订阅/取消订阅
 *   - QOS1/QOS2消息确认机制
 *   - CMD指令处理(OneNET扩展)
 *   - 内存管理(动态/静态)
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/
#ifndef _MQTTKIT_H_
#define _MQTTKIT_H_

#include "Common.h"

/*=============================内存分配方式====================================*/
/*===========可提供RTOS的内存分配方式，也可以直接使用C标准库=====================*/
#include <stdlib.h>

#define MQTT_MallocBuffer	malloc
#define MQTT_FreeBuffer		free

/*============================================================================*/

/* 高低字节转换宏 */
#define MOSQ_MSB(A)         (uint8)((A & 0xFF00) >> 8)
#define MOSQ_LSB(A)         (uint8)(A & 0x00FF)

/*--------------------------------内存分配方式标志--------------------------------*/
#define MEM_FLAG_NULL		0
#define MEM_FLAG_ALLOC		1
#define MEM_FLAG_STATIC		2

/****************************************************************************
 * 结构体: MQTT_PACKET_STRUCTURE
 * 功能:   MQTT报文缓冲区管理结构体
 * 成员:   _data - 缓冲区数据指针
 *         _len - 已写入数据长度
 *         _size - 缓冲区总大小
 *         _memFlag - 内存使用方式(0-未使用, 1-动态分配, 2-静态分配)
 ****************************************************************************/
typedef struct Buffer
{
    uint8	*_data;		//缓冲区数据
    uint32	_len;		//已写入数据长度
    uint32	_size;		//缓冲区总大小
    uint8	_memFlag;	//内存使用方式
} MQTT_PACKET_STRUCTURE;

/*--------------------------------固定头报文类型定义--------------------------------*/
enum MqttPacketType
{
    MQTT_PKT_CONNECT = 1,    /**< 客户端连接请求包 */
    MQTT_PKT_CONNACK,        /**< 连接确认数据包 */
    MQTT_PKT_PUBLISH,        /**< 发布消息数据包 */
    MQTT_PKT_PUBACK,         /**< 发布确认数据包 */
    MQTT_PKT_PUBREC,         /**< 发布收到数据包(QoS 2时响应MQTT_PKT_PUBLISH) */
    MQTT_PKT_PUBREL,         /**< 发布释放数据包(QoS 2时响应MQTT_PKT_PUBREC) */
    MQTT_PKT_PUBCOMP,        /**< 发布完成数据包(QoS 2时响应MQTT_PKT_PUBREL) */
    MQTT_PKT_SUBSCRIBE,      /**< 订阅请求包 */
    MQTT_PKT_SUBACK,         /**< 订阅确认包 */
    MQTT_PKT_UNSUBSCRIBE,    /**< 取消订阅请求包 */
    MQTT_PKT_UNSUBACK,       /**< 取消订阅确认包 */
    MQTT_PKT_PINGREQ,        /**< ping 请求包 */
    MQTT_PKT_PINGRESP,       /**< ping 响应包 */
    MQTT_PKT_DISCONNECT,     /**< 断开连接请求包 */

    MQTT_PKT_CMD             /**< 自定义CMD数据包(OneNET扩展) */
};

/*--------------------------------MQTT QOS等级--------------------------------*/
enum MqttQosLevel
{
    MQTT_QOS_LEVEL0,  /**< 最多传送一次 */
    MQTT_QOS_LEVEL1,  /**< 至少传送一次 */
    MQTT_QOS_LEVEL2   /**< 只传送一次 */
};

/*--------------------------------MQTT连接参数标志位(内部使用)--------------------------------*/
enum MqttConnectFlag
{
    MQTT_CONNECT_CLEAN_SESSION  = 0x02,
    MQTT_CONNECT_WILL_FLAG      = 0x04,
    MQTT_CONNECT_WILL_QOS0      = 0x00,
    MQTT_CONNECT_WILL_QOS1      = 0x08,
    MQTT_CONNECT_WILL_QOS2      = 0x10,
    MQTT_CONNECT_WILL_RETAIN    = 0x20,
    MQTT_CONNECT_PASSORD        = 0x40,
    MQTT_CONNECT_USER_NAME      = 0x80
};

/*--------------------------------消息packet ID(可自定义)--------------------------------*/
#define MQTT_PUBLISH_ID			10
#define MQTT_SUBSCRIBE_ID		20
#define MQTT_UNSUBSCRIBE_ID		30

/*--------------------------------缓冲区管理--------------------------------*/
void MQTT_DeleteBuffer(MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------报文解析--------------------------------*/
uint8 MQTT_UnPacketRecv(uint8 *dataPtr);

/*--------------------------------连接请求--------------------------------*/
uint8 MQTT_PacketConnect(const int8 *user, const int8 *password, const int8 *devid,
                        uint16 cTime, uint1 clean_session, uint1 qos,
                        const int8 *will_topic, const int8 *will_msg, int32 will_retain,
                        MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------断开连接请求--------------------------------*/
uint1 MQTT_PacketDisConnect(MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------连接响应解析--------------------------------*/
uint8 MQTT_UnPacketConnectAck(uint8 *rev_data);

/*--------------------------------数据点上传请求--------------------------------*/
uint1 MQTT_PacketSaveData(const int8 *pro_id, const char *dev_name,
                            int16 send_len, int8 *type_bin_head, MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------二进制文件上传请求--------------------------------*/
uint1 MQTT_PacketSaveBinData(const int8 *name, int16 file_len, MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------CMD指令解析--------------------------------*/
uint8 MQTT_UnPacketCmd(uint8 *rev_data, int8 **cmdid, int8 **req, uint16 *req_len);

/*--------------------------------CMD响应请求--------------------------------*/
uint1 MQTT_PacketCmdResp(const int8 *cmdid, const int8 *req, MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------订阅请求--------------------------------*/
uint8 MQTT_PacketSubscribe(uint16 pkt_id, enum MqttQosLevel qos, const int8 *topics[], uint8 topics_cnt, MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------订阅响应解析--------------------------------*/
uint8 MQTT_UnPacketSubscribe(uint8 *rev_data);

/*--------------------------------取消订阅请求--------------------------------*/
uint8 MQTT_PacketUnSubscribe(uint16 pkt_id, const int8 *topics[], uint8 topics_cnt, MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------取消订阅响应解析--------------------------------*/
uint1 MQTT_UnPacketUnSubscribe(uint8 *rev_data);

/*--------------------------------发布消息请求--------------------------------*/
uint8 MQTT_PacketPublish(uint16 pkt_id, const int8 *topic,
                        const int8 *payload, uint32 payload_len,
                        enum MqttQosLevel qos, int32 retain, int32 own,
                        MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------发布消息响应解析--------------------------------*/
uint8 MQTT_UnPacketPublish(uint8 *rev_data, int8 **topic, uint16 *topic_len, int8 **payload, uint16 *payload_len, uint8 *qos, uint16 *pkt_id);

/*--------------------------------发布消息Ack请求--------------------------------*/
uint1 MQTT_PacketPublishAck(uint16 pkt_id, MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------发布消息Ack解析--------------------------------*/
uint1 MQTT_UnPacketPublishAck(uint8 *rev_data);

/*--------------------------------发布消息Rec请求--------------------------------*/
uint1 MQTT_PacketPublishRec(uint16 pkt_id, MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------发布消息Rec解析--------------------------------*/
uint1 MQTT_UnPacketPublishRec(uint8 *rev_data);

/*--------------------------------发布消息Rel请求--------------------------------*/
uint1 MQTT_PacketPublishRel(uint16 pkt_id, MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------发布消息Rel解析--------------------------------*/
uint1 MQTT_UnPacketPublishRel(uint8 *rev_data, uint16 pkt_id);

/*--------------------------------发布消息Comp请求--------------------------------*/
uint1 MQTT_PacketPublishComp(uint16 pkt_id, MQTT_PACKET_STRUCTURE *mqttPacket);

/*--------------------------------发布消息Comp解析--------------------------------*/
uint1 MQTT_UnPacketPublishComp(uint8 *rev_data);

/*--------------------------------Ping请求--------------------------------*/
uint1 MQTT_PacketPing(MQTT_PACKET_STRUCTURE *mqttPacket);

#endif
