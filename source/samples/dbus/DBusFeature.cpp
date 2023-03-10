// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "DBusFeature.h"
#include "../../logging/LoggerFactory.h"
#include "../../util/FileUtils.h"

#include <aws/common/byte_buf.h>
#include <aws/crt/Api.h>
#include <aws/iotdevicecommon/IotDevice.h>
#include <iostream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <errno.h>

#include <sys/inotify.h>

using namespace std;
using namespace Aws;
using namespace Aws::Iot;
using namespace Aws::Crt;
using namespace Aws::Iot::DeviceClient;
using namespace Aws::Crt::Mqtt;
using namespace Aws::Iot::DeviceClient::Samples;
using namespace Aws::Iot::DeviceClient::Util;
using namespace Aws::Iot::DeviceClient::Logging;

constexpr char DBusFeature::TAG[];
constexpr char DBusFeature::NAME[];

string DBusFeature::getName()
{
    return NAME;
}

bool DBusFeature::createDBus(const PlainConfig &config, const std::string &filePath, const aws_byte_buf *payload)
    const
{
    return true;
}

int DBusFeature::init(
    shared_ptr<SharedCrtResourceManager> manager,
    shared_ptr<ClientBaseNotifier> notifier,
    const PlainConfig &config)
{
    resourceManager = manager;
    baseNotifier = notifier;
    thingName = *config.thingName;
    subTopic = config.dBus.subTopic.value();
    subPort = config.dBus.subPort.value();
    pubTopic = config.dBus.pubTopic.value();
    pubPort = config.dBus.pubPort.value();

    return AWS_OP_SUCCESS;
}

int DBusFeature::start()
{
    int rc;

    LOGM_INFO(TAG, "Starting %s", getName().c_str());
    LOGM_INFO(TAG, "%s -> mqtt://%s", pubPort.c_str(), pubTopic.c_str());
    LOGM_INFO(TAG, "%s <- mqtt://%s", subPort.c_str(), subTopic.c_str());

    subContext = zmq_ctx_new();
    subResponder = zmq_socket (subContext, ZMQ_PUB); // yes it's correct
    rc = zmq_bind (subResponder, subPort.c_str());
    if (rc != 0) {
        LOGM_ERROR(TAG, "Error binding sub to %s (%s)", subPort.c_str(), strerror(errno));
        return AWS_OP_ERR;
    }

    pubContext = zmq_ctx_new();
    pubResponder = zmq_socket (pubContext, ZMQ_SUB); // yes it's correct
    rc = zmq_bind (pubResponder, pubPort.c_str());
    if (rc != 0) {
        LOGM_ERROR(TAG, "Error binding pub to %s (%s)", pubPort.c_str(), strerror(errno));
        return AWS_OP_ERR;
    }

    // subscribe for inbound messages
    auto onSubAck = [this](const MqttConnection &, uint16_t, const String &, QOS, int errorCode) -> void {
        LOGM_DEBUG(TAG, "Subscribing: packetId: %u, error: %d", getName().c_str(), errorCode);
    };

    auto onRecvData = [this](const MqttConnection &, const String &, const ByteBuf &payload) -> void {
        zmq_msg_t msg;
        int rc = zmq_msg_init_data(&msg, payload.buffer, payload.len, NULL, NULL);
        if (rc != 0) {
            LOGM_ERROR(TAG, "Error allocating the reply message (%s)", strerror(errno));
            return;
        }

        LOGM_DEBUG(TAG, "Message from %s: %s", subTopic.c_str(), (char*)zmq_msg_data(&msg));
        rc = zmq_msg_send(&msg, subResponder, 0); 
        if (rc < 0) {
            LOGM_ERROR(TAG, "Error sending the message (%s)", strerror(errno));
        }

        zmq_msg_close(&msg);
    };

    resourceManager->getConnection()->Subscribe(subTopic.c_str(), AWS_MQTT_QOS_AT_LEAST_ONCE, onRecvData, onSubAck);

    // start forwarding thread
    thread dbus_monitor_thread(&DBusFeature::run, this);
    dbus_monitor_thread.detach();

    baseNotifier->onEvent(static_cast<Feature *>(this), ClientBaseEventNotification::FEATURE_STARTED);
    return AWS_OP_SUCCESS;
}

int DBusFeature::stop()
{
    needStop.store(true);

    auto onUnsubscribe = [](const MqttConnection &, uint16_t packetId, int errorCode) -> void {
        LOGM_DEBUG(TAG, "Unsubscribing: packetId: %u, error: %d", packetId, errorCode);
    };

    resourceManager->getConnection()->Unsubscribe(subTopic.c_str(), onUnsubscribe);

    baseNotifier->onEvent(static_cast<Feature *>(this), ClientBaseEventNotification::FEATURE_STOPPED);
    return AWS_OP_SUCCESS;
}

void DBusFeature::run()
{
    zmq_msg_t buffer;
    ByteBuf payload;
    int rc;

    LOG_DEBUG(TAG, "Monitor thread started");

    while (!needStop.load())
    {
        rc = zmq_msg_init(&buffer);
        if (rc != 0) {
            LOGM_ERROR(TAG, "Error allocating the buffer (%s)", strerror(errno));
            continue;
        }

        rc = zmq_msg_recv(&buffer, pubResponder, 0);
        if (rc != 0) {
            LOGM_ERROR(TAG, "Error receiving the message (%s)", strerror(errno));
            continue;
        }

        LOGM_DEBUG(TAG, "Message from %s: %s", pubPort.c_str(), zmq_msg_data(&buffer));

        auto onPublishComplete = [buffer, this](const Mqtt::MqttConnection &, uint16_t, int errorCode) mutable {
            zmq_msg_close(&buffer);
        };

        payload = aws_byte_buf_from_c_str((char*)zmq_msg_data(&buffer));
        resourceManager->getConnection()->Publish(pubTopic.c_str(), AWS_MQTT_QOS_AT_LEAST_ONCE, false, payload, onPublishComplete);
    }

    LOG_DEBUG(TAG, "Monitor thread stopped");

}