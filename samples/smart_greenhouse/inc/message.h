
#ifndef MESSAGE_H_
#define MESSAGE_H_

#include <zephyr.h>
#include <string.h>
#include <stddef.h>

/*
 * Format:
 *
 * Fields: |   SOF    |  len  |   id   |  cmd  |  status | data |
 * nBytes: |   0x55   |   1   |   1    |   1   |   1     |  N   |
 *
 * id: Unique identifier
 * cmd: Command
 * status:  Response status
 * data:    Payload
 * len: Payload Len
 */

typedef enum
{
	CMD_GET_VALUE,
	CMD_SET_VALUE,
	CMD_EVENT,
}command_t;

typedef enum message_status_e
{
	MSG_STATE_SUCCESS,
	MSG_STATE_FAIL,
}message_status_t;

typedef enum
{
    ID_TEMP,
    ID_PRESSURE,
    ID_HUMIDITY,
    ID_LED
}id_t;

typedef union
{
    /* Representation of sensor union*/
    u8_t datagen;
    
    struct {
        u16_t degrees;
    }temp;

    struct {
        u16_t percents;
    }humidity;

    struct{
        u16_t bar;
    }pressure;

}sensor_t;


typedef struct __attribute__((packed, aligned(1)))message_header_s
{
	id_t id;
	command_t cmd;
	message_status_t status;
	u8_t dataLen;
    u8_t *dataPtr;
    sensor_t data;
}message_t;

/*
 * @param head
 * @param data
 * @param msgRaw
 * return  size
 */
//void message_init():

/*
 * @param head
 * @param msgRaw
 * return  size
 */
size_t message_compose(u8_t **msgRaw, message_t *msg);

/*
 * @param rawData
 * return  msg
 */
message_t *message_parse(u8_t *rawData);

/*
 * @param rawData
 * return  message_t *
 */
message_t *message_alloc(u8_t size);

/*
 * @param msg
 */
void message_free(message_t *msg);

#endif
