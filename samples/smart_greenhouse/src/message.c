#include "message.h"

#define INDEXDATA 5
#define MESSAGEMINSIZE INDEXDATA
#define STARTOFFRAME 0x55
#define INDEXSTARTOFFRAME 0
#define INDEXSIZE 1
#define INDEXID 2
#define INDEXCMD  3
#define INDEXSTATUS 4


size_t message_compose(uint8_t **msgRaw, message_t *msg)
{
	uint8_t *rawMsg;

	rawMsg = k_malloc(msg->dataLen + MESSAGEMINSIZE);

	if (rawMsg == NULL)
    {
        goto error;
    } else {
        memset(rawMsg, 0, msg->dataLen + MESSAGEMINSIZE);
    }

	rawMsg[INDEXSTARTOFFRAME] = (uint8_t) STARTOFFRAME;
	rawMsg[INDEXSIZE] = (uint8_t) msg->dataLen;
	rawMsg[INDEXID]  = (uint8_t) msg->id;
	rawMsg[INDEXCMD] = (uint8_t) msg->cmd;
	rawMsg[INDEXSTATUS] = (uint8_t) msg->status;

	if(msg->dataPtr != NULL && msg->dataLen != 0)
	{
		for (uint8_t i = 0; i < msg->dataLen; ++i) {
			rawMsg[INDEXDATA + i] = msg->dataPtr[i];
		}
	}

	*msgRaw = rawMsg;

	if (*msgRaw != NULL) {
		return MESSAGEMINSIZE + msg->dataLen;
	} else {
		goto error;
	}

error:
	k_free(rawMsg);
	return 0;
}

message_t *message_parse(uint8_t *rawData)
{
    message_t *msg;
	uint8_t *temp;

	if (rawData[INDEXSTARTOFFRAME] != STARTOFFRAME)
		goto error;

    msg = message_alloc(rawData[INDEXSIZE] + sizeof(message_t));

    if (msg == NULL)
        goto error;

    msg->dataLen = (uint8_t ) rawData[INDEXSIZE];
	msg->id = (id_t) rawData[INDEXID];
	msg->cmd =  (command_t) rawData[INDEXCMD];
	msg->status = (message_status_t) rawData[INDEXSTATUS];

	temp = k_malloc(msg->dataLen);

	if (temp == NULL)
		goto error;

	memcpy(temp, rawData + INDEXDATA, msg->dataLen);

	msg->dataPtr = temp;

	return msg;

error:
	k_free(temp);
	message_free(msg);
	return NULL;

}

void message_init(message_t *msg)
{
	if(msg != NULL)
	{
		msg->dataPtr = &msg->data.datagen;
	}
}

message_t *message_alloc(uint8_t size)
{
    message_t *msg;
	size_t msgSize = size + sizeof(message_t);

    msg = k_malloc(msgSize);

    if (msg != NULL) {
        memset(msg, 0, msgSize);
    }

	return msg;
}

void message_free(message_t *msg)
{
    k_free(msg);
}
