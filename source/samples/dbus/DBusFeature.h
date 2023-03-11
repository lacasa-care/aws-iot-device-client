// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#ifndef DEVICE_CLIENT_AWSIOTFEATURE_H
#define DEVICE_CLIENT_AWSIOTFEATURE_H

#include <aws/iot/MqttClient.h>
#include <dbus/dbus.h>
#include <zmq.h>

#include "../../ClientBaseNotifier.h"
#include "../../Feature.h"
#include "../../SharedCrtResourceManager.h"
#include "../../config/Config.h"
#include "../../util/FileUtils.h"

namespace Aws
{
    namespace Iot
    {
        namespace DeviceClient
        {
            namespace Samples
            {
                class DBusFeature : public Feature
                {
                  public:
                    static constexpr char NAME[] = "DBus Client";
                    bool createDBus(
                        const PlainConfig &config,
                        const std::string &absFilePath,
                        const aws_byte_buf *payload) const;
                    /**
                     * \brief Initializes the PubSub feature with all the required setup information, event
                     * handlers, and the SharedCrtResourceManager
                     *
                     * @param manager The resource manager used to manage CRT resources
                     * @param notifier an ClientBaseNotifier used for notifying the client base of events or errors
                     * @param config configuration information passed in by the user via either the command line or
                     * configuration file
                     * @return a non-zero return code indicates a problem. The logs can be checked for more info
                     */
                    int init(
                        std::shared_ptr<SharedCrtResourceManager> manager,
                        std::shared_ptr<ClientBaseNotifier> notifier,
                        const PlainConfig &config);
                    void LoadFromConfig(const PlainConfig &config);

                    // Interface methods defined in Feature.h
                    std::string getName() override;
                    int start() override;
                    int stop() override;

                  private:
                    /**
                     * \brief the ThingName to use
                     */
                    std::string thingName;
                    static constexpr char TAG[] = "samples/DBusFeature.cpp";

                    /**
                     * \brief The resource manager used to manage CRT resources
                     */
                    std::shared_ptr<SharedCrtResourceManager> resourceManager;
                    /**
                     * \brief An interface used to notify the Client base if there is an event that requires its
                     * attention
                     */
                    std::shared_ptr<ClientBaseNotifier> baseNotifier;
                    /**
                     * \brief Whether the DeviceClient base has requested this feature to stop.
                     */
                    std::atomic<bool> needStop{false};
                    /**
                     * \brief DBus connection object.
                     */
                    DBusConnection* conn;
                    /**
                     * \brief A send thread.
                     */
                    void run();

                    void *context;
                    void *responder;
                    std::string inboundTopic;
                    std::string outboundTopic;
                    std::string port;

                };
            } // namespace Samples
        }     // namespace DeviceClient
    }         // namespace Iot
} // namespace Aws

#endif // DEVICE_CLIENT_SAMPLESFEATURE_H
