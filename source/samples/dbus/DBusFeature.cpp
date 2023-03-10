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

    return AWS_OP_SUCCESS;
}

int DBusFeature::start()
{
    DBusError err;
    int ret;

    LOGM_INFO(TAG, "Starting %s", getName().c_str());

    // check the DBUS_SESSION_BUS_ADDRESS to verify the DBus daemon
    if (const char* env_p = std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        LOGM_DEBUG(TAG, "DBus daemon is running (%s)", env_p);
    } else {
        LOG_ERROR(TAG, "DBus daemon is not running, exiting");
        return AWS_OP_ERR;
    } 

    // initialise the error value
    dbus_error_init(&err);

    // connect to the DBUS system bus, and check for errors
    conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) { 
        LOGM_ERROR(TAG, "Connection Error (%s)", err.message);
        dbus_error_free(&err); 
        return AWS_OP_ERR;
    }

    // register our name on the bus, and check for errors
    ret = dbus_bus_request_name(conn, "aws.iot.device.Client", DBUS_NAME_FLAG_REPLACE_EXISTING , &err);
    if (dbus_error_is_set(&err)) { 
        LOGM_ERROR(TAG, "Registration Error (%s)", err.message);
        dbus_error_free(&err); 
        return AWS_OP_ERR;
    }

    if (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != ret) { 
        LOGM_ERROR(TAG, "Owner Error (%d)", ret);
        return AWS_OP_ERR;
    }

    // start processing thread
    thread dbus_monitor_thread(&DBusFeature::runDBusMonitor, this);
    dbus_monitor_thread.detach();

    baseNotifier->onEvent(static_cast<Feature *>(this), ClientBaseEventNotification::FEATURE_STARTED);
    return AWS_OP_SUCCESS;
}

int DBusFeature::stop()
{
    needStop.store(true);

    baseNotifier->onEvent(static_cast<Feature *>(this), ClientBaseEventNotification::FEATURE_STOPPED);
    return AWS_OP_SUCCESS;
}

void DBusFeature::runDBusMonitor()
{
    DBusMessage* msg;
    DBusMessageIter args;
    char* param;
    ByteBuf payload;
    dbus_uint32_t serial = 0;
    DBusMessage* reply;
    const char* response = "hello from aws";

    LOG_DEBUG(TAG, "DBus monitor thread started");

    while (!needStop.load())
    {
        // non blocking read of the next available message
        dbus_connection_read_write(conn, 0);
        msg = dbus_connection_pop_message(conn);

        // loop again if we haven't read a message
        if (NULL == msg) { 
            this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // check this is a method call for the right interface & method
        if (dbus_message_is_method_call(msg, "aws.iot.device.Publisher", "publish")) {
            if (!dbus_message_iter_init(msg, &args)) {
                LOG_DEBUG(TAG, "Message has no arguments, ignoring");
            } else if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type(&args)) {
                LOG_DEBUG(TAG, "Message argument is not a string, ignoring");
            } else {
                dbus_message_iter_get_basic(&args, &param);
                LOGM_DEBUG(TAG, "Got a call: %s", param);
                payload = aws_byte_buf_from_c_str(param);

                auto onPublishComplete = [payload, this](const Mqtt::MqttConnection &, uint16_t, int errorCode) mutable {
                    LOGM_DEBUG(TAG, "PublishCompAck: PacketId:(%s), ErrorCode:%d", getName().c_str(), errorCode);
                    aws_byte_buf_clean_up_secure(&payload);
                };

                resourceManager->getConnection()->Publish("lacasa", AWS_MQTT_QOS_AT_LEAST_ONCE, false, payload, onPublishComplete);

                // create a reply from the message
                reply = dbus_message_new_method_return(msg);

                // add the arguments to the reply
                dbus_message_iter_init_append(reply, &args);
                if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &response)) { 
                    LOG_DEBUG(TAG, "Cannot add argument to the reply");
                    return;
                }

                // send the reply && flush the connection
                if (!dbus_connection_send(conn, reply, &serial)) {
                    LOG_DEBUG(TAG, "Cannot send reply");
                    return;
                }

                LOGM_DEBUG(TAG, "Reply delivered: %d", serial);
                dbus_connection_flush(conn);

                // free the reply
                dbus_message_unref(reply);

            }
        }

        // free the message
        dbus_message_unref(msg);
    }

    LOG_DEBUG(TAG, "DBus monitor thread stopped");

    if (NULL != conn) { 
        dbus_connection_close(conn);
    }

}