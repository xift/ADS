#include "AmsConnection.h"
#include "Log.h"

AmsResponse::AmsResponse()
    : frame(4096),
    invokeId(0)
{}

void AmsResponse::Notify()
{
    invokeId = 0;
    cv.notify_all();
}

bool AmsResponse::Wait(uint32_t timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
        return !invokeId;
    });
}

std::shared_ptr<NotificationDispatcher> AmsConnection::DispatcherList::Add(const VirtualConnection& connection,
                                                                           AmsProxy&                proxy)
{
    const auto dispatcher = Get(connection);
    if (dispatcher) {
        return dispatcher;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return list.emplace(connection,
                        std::make_shared<NotificationDispatcher>(proxy, connection.second,
                                                                 connection.first)).first->second;
}

std::shared_ptr<NotificationDispatcher> AmsConnection::DispatcherList::Get(const VirtualConnection& connection)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    const auto it = list.find(connection);
    if (it != list.end()) {
        return it->second;
    }
    return std::shared_ptr<NotificationDispatcher> {}
}

AmsConnection::AmsConnection(Router& __router, IpV4 destIp)
    : destIp(destIp),
    router(__router),
    socket(destIp, 48898),
    invokeId(0)
{
    socket.Connect();
    receiver = std::thread(&AmsConnection::TryRecv, this);
}

AmsConnection::~AmsConnection()
{
    socket.Shutdown();
    receiver.join();
}

NotificationId AmsConnection::CreateNotifyMapping(uint16_t               port,
                                                  AmsAddr                addr,
                                                  PAdsNotificationFuncEx pFunc,
                                                  uint32_t               hUser,
                                                  uint32_t               length,
                                                  uint32_t               hNotify)
{
    const auto dispatcher = dispatcherList.Add(VirtualConnection { port, addr }, *this);
    dispatcher->Emplace(pFunc, hUser, length, hNotify);
    return NotificationId { hNotify, dispatcher };
}

long AmsConnection::DeleteNotification(const AmsAddr& amsAddr, uint32_t hNotify, uint32_t tmms, uint16_t port)
{
    Frame request(sizeof(AmsTcpHeader) + sizeof(AoEHeader) + sizeof(hNotify));
    request.prepend(qToLittleEndian(hNotify));
    return AdsRequest<AoEResponseHeader>(request, amsAddr, tmms, port, AoEHeader::DEL_DEVICE_NOTIFICATION);
}

AmsResponse* AmsConnection::Write(Frame& request, const AmsAddr destAddr, const AmsAddr srcAddr, uint16_t cmdId)
{
    AoEHeader aoeHeader { destAddr.netId, destAddr.port, srcAddr.netId, srcAddr.port, cmdId,
                          static_cast<uint32_t>(request.size()), GetInvokeId() };
    request.prepend<AoEHeader>(aoeHeader);

    AmsTcpHeader header { static_cast<uint32_t>(request.size()) };
    request.prepend<AmsTcpHeader>(header);

    auto response = Reserve(aoeHeader.invokeId(), srcAddr.port);

    if (!response) {
        return nullptr;
    }

    if (request.size() != socket.write(request)) {
        Release(response);
        return nullptr;
    }
    return response;
}

uint32_t AmsConnection::GetInvokeId()
{
    uint32_t result;
    do {
        result = invokeId.fetch_add(1);
    } while (!result);
    return result;
}

AmsResponse* AmsConnection::GetPending(uint32_t id, uint16_t port)
{
    const uint32_t currentId = queue[port - Router::PORT_BASE].invokeId;
    if (currentId == id) {
        return &queue[port - Router::PORT_BASE];
    }
    LOG_WARN("InvokeId missmatch: waiting for 0x" << std::hex << currentId << " received 0x" << id);
    return nullptr;
}

AmsResponse* AmsConnection::Reserve(uint32_t id, uint16_t port)
{
    uint32_t isFree = 0;
    if (!queue[port - Router::PORT_BASE].invokeId.compare_exchange_weak(isFree, id)) {
        LOG_WARN("Port: " << port << " already in use as " << isFree);
        return nullptr;
    }
    return &queue[port - Router::PORT_BASE];
}

void AmsConnection::Release(AmsResponse* response)
{
    response->frame.reset();
    response->invokeId = 0;
}

void AmsConnection::Receive(void* buffer, size_t bytesToRead) const
{
    auto pos = reinterpret_cast<uint8_t*>(buffer);
    while (bytesToRead) {
        const size_t bytesRead = socket.read(pos, bytesToRead);
        bytesToRead -= bytesRead;
        pos += bytesRead;
    }
}

void AmsConnection::ReceiveJunk(size_t bytesToRead) const
{
    uint8_t buffer[1024];
    while (bytesToRead > sizeof(buffer)) {
        Receive(buffer, sizeof(buffer));
        bytesToRead -= sizeof(buffer);
    }
    Receive(buffer, bytesToRead);
}

Frame& AmsConnection::ReceiveFrame(Frame& frame, size_t bytesLeft) const
{
    if (bytesLeft > frame.capacity()) {
        LOG_WARN("Frame to long: " << std::dec << bytesLeft << '<' << frame.capacity());
        ReceiveJunk(bytesLeft);
        return frame.clear();
    }
    Receive(frame.rawData(), bytesLeft);
    return frame.limit(bytesLeft);
}

bool AmsConnection::ReceiveNotification(const AoEHeader& header)
{
    const auto dispatcher = dispatcherList.Get(VirtualConnection { header.targetPort(), header.sourceAms() });
    if (!dispatcher) {
        ReceiveJunk(header.length());
        LOG_WARN("No dispatcher found for notification");
        return false;
    }

    auto& ring = dispatcher->ring;
    auto bytesLeft = header.length();
    if (bytesLeft > ring.BytesFree()) {
        ReceiveJunk(bytesLeft);
        LOG_WARN("port " << std::dec << header.targetPort() << " receive buffer was full");
        return false;
    }

    auto chunk = ring.WriteChunk();
    while (bytesLeft > chunk) {
        Receive(ring.write, chunk);
        ring.Write(chunk);
        bytesLeft -= chunk;
        chunk = ring.WriteChunk();
    }
    Receive(ring.write, bytesLeft);
    ring.Write(bytesLeft);
    dispatcher->Notify();
    return true;
}

void AmsConnection::TryRecv()
{
    try {
        Recv();
    }catch (std::runtime_error& e) {
        LOG_INFO(e.what());
    }
}

void AmsConnection::Recv()
{
    AmsTcpHeader amsTcpHeader;
    AoEHeader aoeHeader;
    for ( ; ; ) {
        Receive(amsTcpHeader);
        if (amsTcpHeader.length() < sizeof(aoeHeader)) {
            LOG_WARN("Frame to short to be AoE");
            ReceiveJunk(amsTcpHeader.length());
            continue;
        }

        Receive(aoeHeader);
        if (aoeHeader.cmdId() == AoEHeader::DEVICE_NOTIFICATION) {
            ReceiveNotification(aoeHeader);
            continue;
        }

        auto response = GetPending(aoeHeader.invokeId(), aoeHeader.targetPort());
        if (!response) {
            LOG_WARN("No response pending");
            ReceiveJunk(aoeHeader.length());
            continue;
        }

        ReceiveFrame(response->frame, aoeHeader.length());

        switch (aoeHeader.cmdId()) {
        case AoEHeader::READ_DEVICE_INFO:
        case AoEHeader::READ:
        case AoEHeader::WRITE:
        case AoEHeader::READ_STATE:
        case AoEHeader::WRITE_CONTROL:
        case AoEHeader::ADD_DEVICE_NOTIFICATION:
        case AoEHeader::DEL_DEVICE_NOTIFICATION:
        case AoEHeader::READ_WRITE:
            break;

        default:
            LOG_WARN("Unkown AMS command id");
            response->frame.clear();
        }

        response->Notify();
    }
}
