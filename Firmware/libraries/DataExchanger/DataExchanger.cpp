#include <DataExchanger.h>

Message::~Message() {
}

size_t Message::writeTo(DataStreamWriter *dsw) {
    m_crc = calculateCrc();
    size_t r = 0;
    r += dsw->writeShort(m_crc);
    r += dsw->writeByte(m_type);
    r += dsw->writeByte(m_status);
    r += dsw->writeArray(m_fromId, ID_DATA_LENGTH);
    r += dsw->writeArray(m_targetId, ID_DATA_LENGTH);
    r += dsw->writeArray(m_data, CUSTOM_MESSAGE_DATA_LENGTH);
    if (r != MESSAGE_SIZE) {
        return -1;
    }
    return r;
}

size_t Message::readFrom(DataStreamReader *dsr) {
    bool ok = true;
    m_crc = dsr->readShort(&ok);
    size_t r = 2;
    if (!ok) return -1;

    m_type = dsr->readByte(&ok);
    r++;
    if (!ok) return -1;

    m_status = dsr->readByte(&ok);
    r++;
    if (!ok) return -1;

    dsr->readFully(m_fromId, ID_DATA_LENGTH, &ok);
    r += ID_DATA_LENGTH;
    if (!ok) return -1;

    dsr->readFully(m_targetId, ID_DATA_LENGTH, &ok);
    r += ID_DATA_LENGTH;
    if (!ok) return -1;

    dsr->readFully(m_data, CUSTOM_MESSAGE_DATA_LENGTH, &ok);
    r += CUSTOM_MESSAGE_DATA_LENGTH;
    if (!ok) return -1;

    if (r != MESSAGE_SIZE) {
        return -1;
    }
    return r;
}

uint16_t Message::calculateCrc() {
    byte buffer[MESSAGE_SIZE];
    buffer[0] = (byte)(m_crc >> 8);
    buffer[1] = (byte)(m_crc >> 0);
    buffer[2] = m_type;
    buffer[3] = m_status;
    Utils::copyArray(m_fromId, buffer+4, ID_DATA_LENGTH);
    Utils::copyArray(m_targetId, buffer+4+ID_DATA_LENGTH, ID_DATA_LENGTH);
    Utils::copyArray(m_data, buffer+4+2*ID_DATA_LENGTH, CUSTOM_MESSAGE_DATA_LENGTH);
    return SimpleCrc::crc16(buffer, MESSAGE_SIZE);
}

void Message::swapIds() {
    // Swap fromId and targetId arrays.
    byte buffer[ID_DATA_LENGTH];
    Utils::copyArray(m_fromId, buffer, ID_DATA_LENGTH);
    Utils::copyArray(m_targetId, m_fromId, ID_DATA_LENGTH);
    Utils::copyArray(buffer, m_targetId, ID_DATA_LENGTH);
}

Handler::~Handler() {
}

bool SerialOutputHandler::handleMessage(Message *message) {
    Serial.print("crc16\t:"); Serial.println(message->m_crc);
    Serial.print("type\t:");  Serial.println(message->m_type);
    Serial.print("status\t:");Serial.println(message->m_status);
    Serial.print("fromId\t:");
    char buff[2];
    for(size_t i = 0; i < ID_DATA_LENGTH; i++) {
        Utils::toHex(buff, message->m_fromId[i]);
        Serial.print(" ");
        Serial.print(buff[0]);
        Serial.print(buff[2]);
    }
    Serial.println();
    for(size_t i = 0; i < ID_DATA_LENGTH; i++) {
        Utils::toHex(buff, message->m_targetId[i]);
        Serial.print(" ");
        Serial.print(buff[0]);
        Serial.print(buff[2]);
    }
    Serial.println();
    for(size_t i = 0; i < ID_DATA_LENGTH; i++) {
        Utils::toHex(buff, message->m_data[i]);
        Serial.print(" ");
        Serial.print(buff[0]);
        Serial.print(buff[2]);
    }
    Serial.println();
    return false; // This message is consumed, and does not generate a response.
}

void DataExchanger::process(
        Message *message,                 // Message received.
        DataStreamWriter *readFromLine,   // Communication line where the message was read from.
        DataStreamWriter *opposingLine) { // Opposing communication line, to crosstalk messages.
    switch(message->m_type) {
    /**
     * The TYPE_SCAN is an special message sent through the chain to discover new devices. This
     * message has a sender id (master id), but no target id. As soon as a board receives this
     * message, it should pass it unchanged to the next board in the line (so that board can respond
     * the scan query), and it should generate a scan message response. The response is the same
     * scan message, but the sender now becomes the slave id, and the target becomes the original
     * sender id (master id).
     */
    case TYPE_SCAN:
        // If I receive an unaddressed scan message, respond to it, and also pass it onto the next
        // in the chain.
        if (Utils::arrayEquals(message->m_targetId, "\0\0\0\0", ID_DATA_LENGTH)) { // No id.
            // Transmit the same unaddressed scan message to the next in the chain.
            transmit(opposingLine, message);

            // Put my id as the targetId.
            Utils::copyArray(m_id, message->m_targetId, ID_DATA_LENGTH);
            // Swap ids to send the message back (I'm sending a message addressed to master).
            message->swapIds();

            // Send back.
            transmit(readFromLine, message);
        }
        // Somebody sent me a response of a scan message that I sent earlier before.
        else if (Utils::arrayEquals(message->m_targetId, m_id, ID_DATA_LENGTH)) {
            // This code is usually invoked by a master device instance, because the master is who
            // queries other devices about their ids. This won't require another response, as it is
            // a response already.
            // This can be extended to perform discovery on more complex topologies :)
            m_handler->handleMessage(message);
        }
        // If I receive a scan message with a targetId set, that means somebody else is responding
        // an scan request. Pass it on, using the opposing communication line from where I received
        // it.
        else {
            transmit(opposingLine, message);
            return;
        }
        break;
    /**
     * The TYPE_DATA is a regular message to send information between devices that know each other
     * ids. Generally, the fromId field is the master id, and the targetId is a slave device id.
     * When the device sends a response back, those ids should be swapped. Depending on the content
     * of the message, the slave device may or may not send back a response (generally, commands do
     * not require a response, while a get request will require). See the class Handler for details
     * on how to process messages and stuff.
     */
    case TYPE_DATA:
        // If data message is addressed to me, process and maybe send response back to same
        // communication line where I received the message.
        if (Utils::arrayEquals(message->m_targetId, m_id, ID_DATA_LENGTH)) {
            if(m_handler->handleMessage(message)) {
                message->swapIds();
                transmit(readFromLine, message);
            }
        }
        // If the message is addressed to someone else, pass it on to the next in the line (transmit
        // to the opposing communication line).
        else {
            transmit(opposingLine, message);
        }
        break;
    }
}

void DataExchanger::transmit(DataStreamWriter *dsw, Message *message) {
    dsw->writeObject(message);
}

DataExchanger::DataExchanger() :
        m_hardwareReader(NULL),
        m_hardwareWriter(NULL),
        m_softwareReader(NULL),
        m_softwareWriter(NULL),
        m_handler(NULL)
{}

void DataExchanger::setup(byte *id, Handler *handler) {
    Utils::copyArray(id, m_id, ID_DATA_LENGTH);
    m_handler = handler;
}

void DataExchanger::setupHardware(DataStreamReader *dsr, DataStreamWriter *dsw) {
    m_hardwareReader = dsr;
    m_hardwareWriter = dsw;
}

void DataExchanger::setupSoftware(DataStreamReader *dsr, DataStreamWriter *dsw) {
    m_softwareReader = dsr;
    m_softwareWriter = dsw;
}

void DataExchanger::loop() {
    bool ok = true;
    Message m;
    if (m_hardwareReader && m_hardwareReader->available() >= MESSAGE_SIZE) {
        m_hardwareReader->readObject(&m, &ok);
        if (ok) {
            process(&m, m_hardwareWriter, m_softwareWriter);
        } else {
            // TODO(rtapiapincheira): handle this error!
            // do nothing...
        }
    }
    if (m_softwareReader && m_softwareReader->available() >= MESSAGE_SIZE) {
        m_softwareReader->readObject(&m, &ok);
        if (ok) {
            process(&m, m_softwareWriter, m_hardwareWriter);
        } else {
            // TODO(rtapiapincheira): handle this error!
            // do nothing...
        }
    }
}