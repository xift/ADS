#pragma once

#include "AdsHandle.h"
#include "AdsRoute.h"

struct AdsNotification {
    static AdsNotification Register(const AdsRoute&              route,
                                    const std::string&           symbolName,
                                    const AdsNotificationAttrib& notificationAttributes,
                                    PAdsNotificationFuncEx       callback);

    static AdsNotification Register(const AdsRoute&              route,
                                    uint32_t                     indexGroup,
                                    uint32_t                     indexOffset,
                                    const AdsNotificationAttrib& notificationAttributes,
                                    PAdsNotificationFuncEx       callback);
private:
    AdsNotification(std::shared_ptr<uint32_t> hNotify, AdsHandle hSymbol)
        : m_Notification(hNotify),
        m_Symbol(std::move(hSymbol))
    {}

    std::shared_ptr<uint32_t> m_Notification;
    AdsHandle m_Symbol;
};

/*
    Doesn't work. Connection to host is closed as soon as the device goes back to config-mode.


   class AdsDeviceStatusChangedNotification : public AdsNotification
   {
   public:
    AdsDeviceStatusChangedNotification(const AdsRoute& route, std::function<void()> callback)
        :AdsNotification(route, ADSIGRP_DEVICE_DATA, ADSIOFFS_DEVDATA_ADSSTATE, GetNotificationAttributes(), callback)
    {
    }

   private:
    AdsNotificationAttrib GetNotificationAttributes()
    {
        AdsNotificationAttrib notificationAttributes;
        memset(&notificationAttributes, 0, sizeof(notificationAttributes));
        notificationAttributes.cbLength = sizeof(uint16_t);
        notificationAttributes.nMaxDelay = 0;
        notificationAttributes.nTransMode = ADSTRANS_SERVERONCHA;
        return notificationAttributes;
    }
   };

 */
