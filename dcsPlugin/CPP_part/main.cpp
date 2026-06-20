/****************************************************************************
 * DCS → Little Navmap 共享内存桥接程序
 *
 * 功能:
 *   1. 接收本地回环 (127.0.0.1:54382) 的 UDP 包（DCS Export.lua 发送）
 *   2. 将数据写入共享内存 "LittleXpconnect"，伪装为 Little Xpconnect 插件
 *      格式，使 Little Navmap 可以读取 DCS 中的飞机位置数据
 *
 * 协议:
 *   - UDP 包格式 (LittleEndian): HEADER(0x1ab2f407, 4B) + count(4B) + [每条记录 36B]
 *     每条记录: id(4B) + name(8B) + alt_ft(4B) + heading_deg(4B) + lat_rad(8B) + lon_rad(8B)
 *   - 共享内存格式 (BigEndian, QDataStream::Qt_5_5):
 *     totalSize(4B) + terminate(4B) + SimConnectData payload
 ****************************************************************************/

#include <QCoreApplication>
#include <QUdpSocket>
#include <QSharedMemory>
#include <QDataStream>
#include <QDateTime>
#include <QDebug>

#include <cmath>
#include <limits>

// ============ 常量定义（与 Little Xpconnect / atools 一致） ============
constexpr quint32 MAGIC_NUMBER_DATA  = 0xF75E0AF3;
constexpr quint32 DATA_VERSION       = 11;
constexpr const char *SHARED_MEMORY_KEY = "LittleXpconnect";
constexpr int SHARED_MEMORY_SIZE     = 8196;
constexpr quint16 UDP_PORT           = 54382;
constexpr quint32 DCS_HEADER         = 0x1ab2f407;
constexpr float  SC_INVALID_FLOAT    = std::numeric_limits<float>::max();

// DCS 对象类型 ID
constexpr quint32 DCS_ID_PLANE     = 1;
constexpr quint32 DCS_ID_HELICOPTER = 2;
constexpr quint32 DCS_ID_WARSHIP   = 3;
constexpr quint32 DCS_ID_CARRIER   = 4;
constexpr quint32 DCS_ID_GROUND    = 5;

// SimConnect Category 枚举
constexpr quint8 CAT_AIRPLANE      = 0;
constexpr quint8 CAT_HELICOPTER    = 1;
constexpr quint8 CAT_BOAT          = 2;
constexpr quint8 CAT_CARRIER       = 3;
constexpr quint8 CAT_FRIGATE       = 4;
constexpr quint8 CAT_GROUNDVEHICLE = 5;
constexpr quint8 CAT_UNKNOWN       = 9;

// SimConnect EngineType 枚举
constexpr quint8 ENG_PISTON       = 0;
constexpr quint8 ENG_JET          = 1;
constexpr quint8 ENG_NO_ENGINE    = 2;
constexpr quint8 ENG_HELO_TURBINE = 3;
constexpr quint8 ENG_UNSUPPORTED  = 4;
constexpr quint8 ENG_TURBOPROP    = 5;

// AircraftFlags 位掩码
constexpr quint16 AFLAG_ON_GROUND    = 0x0001;
constexpr quint16 AFLAG_IS_USER      = 0x0010;
constexpr quint16 AFLAG_SIM_XPLANE12 = 0x1000;

// Props 类型常量（对应 ptinternal::PropType）
constexpr quint8 PROP_TYPE_DOUBLE  = 10; // ptinternal::DOUBLE
constexpr quint8 PROP_TYPE_STRING8 = 12; // ptinternal::STRING8

// SimConnectAircraftPropTypes 键
constexpr quint8 PROP_KEY_AIRCRAFT_LONX     = 1;
constexpr quint8 PROP_KEY_AIRCRAFT_LATY     = 2;
constexpr quint8 PROP_KEY_XPCONNECT_VERSION = 3;

// ============ DCS 解析后的对象 ============
struct DcsObject
{
    quint32 id;
    QString name;
    qint32  altFt;
    qint32  headingDeg;
    double  latRad;
    double  lonRad;
};

// ============ 辅助函数 ============

// DCS ID → SimConnect Category
static quint8 dcsIdToCategory(quint32 id)
{
    switch(id)
    {
    case DCS_ID_PLANE:     return CAT_AIRPLANE;
    case DCS_ID_HELICOPTER: return CAT_HELICOPTER;
    case DCS_ID_WARSHIP:   return CAT_BOAT;
    case DCS_ID_CARRIER:   return CAT_CARRIER;
    case DCS_ID_GROUND:    return CAT_GROUNDVEHICLE;
    default:               return CAT_UNKNOWN;
    }
}

// DCS ID → SimConnect EngineType
static quint8 dcsIdToEngineType(quint32 id)
{
    switch(id)
    {
    case DCS_ID_PLANE:     return ENG_JET;
    case DCS_ID_HELICOPTER: return ENG_HELO_TURBINE;
    default:               return ENG_NO_ENGINE;
    }
}

// 写短字符串: quint8 长度 + UTF-8 字节（无 NUL）
static void writeShortString(QDataStream &ds, const QString &str)
{
    QByteArray utf8 = str.toUtf8();
    quint8 len = static_cast<quint8>(qMin(utf8.size(), 255));
    ds << len;
    if(len > 0)
        ds.writeRawData(utf8.constData(), len);
}

// 写单个 Prop (key:quint8, type:quint8, double 值)
static void writePropDouble(QDataStream &ds, quint8 key, double value)
{
    ds << key;
    ds << PROP_TYPE_DOUBLE;
    ds << value;
}

// 写单个 Prop (key:quint8, type:quint8, STRING8 值)
static void writePropString8(QDataStream &ds, quint8 key, const QString &value)
{
    ds << key;
    ds << PROP_TYPE_STRING8;
    QByteArray utf8 = value.toUtf8();
    quint8 len = static_cast<quint8>(qMin(utf8.size(), 255));
    ds << len;
    if(len > 0)
        ds.writeRawData(utf8.constData(), len);
}

// 写空的 Props 容器: quint16 count = 0
static void writePropsEmpty(QDataStream &ds)
{
    ds << quint16(0);
}

// 写用户机 Props（高精度经纬 + 插件版本）
static void writePropsUser(QDataStream &ds, double latDeg, double lonDeg)
{
    ds << quint16(3); // 3 条属性
    writePropDouble(ds, PROP_KEY_AIRCRAFT_LONX, lonDeg);
    writePropDouble(ds, PROP_KEY_AIRCRAFT_LATY, latDeg);
    writePropString8(ds, PROP_KEY_XPCONNECT_VERSION, QStringLiteral("1.2.2"));
}



// 写 SimConnectAircraft 基类字段
static void writeAircraftBase(QDataStream &ds, quint32 objectId, const DcsObject &obj,
                              quint8 category, quint8 engineType, quint16 extraFlags,
                              bool isUser)
{
    double latDeg = obj.latRad * 180.0 / M_PI;
    double lonDeg = obj.lonRad * 180.0 / M_PI;
    float  altFt = static_cast<float>(obj.altFt);
    float  headingDeg = static_cast<float>(obj.headingDeg);

    // P1:  objectId
    ds << objectId;
    // P2:  dataFlags (NO_FLAGS = 0, 包含字符串)
    ds << quint8(0);
    // P3:  flags (AircraftFlags)
    ds << quint16(AFLAG_SIM_XPLANE12 | extraFlags);

    // P4a-h: 8 个短字符串
    writeShortString(ds, obj.name);       // airplaneTitle
    writeShortString(ds, QString());      // airplaneModel
    writeShortString(ds, QString());      // airplaneReg
    writeShortString(ds, QString());      // airplaneType
    writeShortString(ds, QString());      // airplaneAirline
    writeShortString(ds, QString());      // airplaneFlightnumber
    writeShortString(ds, QString());      // fromIdent
    writeShortString(ds, QString());      // toIdent

    // P5-P6: 经纬度 (float)
    ds << static_cast<float>(lonDeg);
    ds << static_cast<float>(latDeg);
    // P7: altitude
    ds << altFt;
    // P8-P9: heading (真/磁)
    ds << headingDeg << headingDeg;
    // P10-P12: 速度
    ds << 0.0f << 0.0f << 0.0f;
    // P13: indicatedAltitudeFt
    ds << altFt;
    // P14-P15: TAS, Mach
    ds << 0.0f << 0.0f;
    // P16: numberOfEngines
    ds << quint8(category <= CAT_HELICOPTER ? 1 : 0);
    // P17-P19: wingSpanFt, modelRadiusFt, deckHeight
    if(category == CAT_CARRIER)
    {
        ds << quint16(1000); // wingSpanFt (航母近似)
        ds << quint16(500);  // modelRadiusFt
        ds << quint16(50);   // deckHeight
    }
    else
    {
        ds << quint16(37);
        ds << quint16(20);
        ds << quint16(0);
    }
    // P20-P22: category, engineType, transponderCode
    ds << category;
    ds << engineType;
    ds << qint16(-1); // transponderCode 无效

    // P23: properties
    if(isUser)
        writePropsUser(ds, latDeg, lonDeg);
    else
        writePropsEmpty(ds);
}

// 写 SimConnectUserAircraft 额外字段（在 SimConnectAircraft 基类之后）
static void writeUserAircraftExtra(QDataStream &ds)
{
    ds << 0.0f;       // windSpeedKts
    ds << 0.0f;       // windDirectionDegT
    ds << 0.0f;       // altitudeAboveGroundFt
    ds << 0.0f;       // groundAltitudeFt
    ds << 0.0f;       // altitudeAutopilotFt
    ds << 15.0f;      // ambientTemperatureCelsius
    ds << 15.0f;      // totalAirTemperatureCelsius
    ds << 1013.25f;   // seaLevelPressureMbar

    // 8 个结冰百分比
    for(int i = 0; i < 8; i++)
        ds << quint8(0);

    ds << 2000.0f;    // airplaneTotalWeightLbs
    ds << 3000.0f;    // airplaneMaxGrossWeightLbs
    ds << 1500.0f;    // airplaneEmptyWeightLbs
    ds << 50.0f;      // fuelTotalQuantityGallons
    ds << 300.0f;     // fuelTotalWeightLbs
    ds << 0.0f;       // fuelFlowPPH
    ds << 0.0f;       // fuelFlowGPH
    ds << 0.0f;       // magVarDeg
    ds << 10000.0f;   // ambientVisibilityMeter
    ds << 0.0f;       // trackMagDeg
    ds << 0.0f;       // trackTrueDeg

    // QDateTime: localDateTime, zuluDateTime
    // 手动按 Qt5 格式序列化 QDateTime，兼容 Little Navmap 3.0.x（Qt 5.15.2）
    // Qt5 格式: qint64 msecsSinceEpoch + int timeSpec(1=UTC)
    // 注意：QDataStream 已设为 BigEndian，用 operator<< 即可写出正确字节序
    qint64 msecs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    ds << msecs;          // localDateTime: msecs
    ds << qint32(1);      // localDateTime: UTC
    ds << msecs;          // zuluDateTime: msecs
    ds << qint32(1);      // zuluDateTime: UTC
}

// 从 DCS 对象列表构建 SimConnectData 载荷字节
static QByteArray buildPayload(const QList<DcsObject> &objects)
{
    QByteArray buffer;
    QDataStream ds(&buffer, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_5);
    ds.setFloatingPointPrecision(QDataStream::SinglePrecision);
    // 字节序默认 BigEndian — 与 Little Xpconnect 一致

    // H1:  magic
    ds << MAGIC_NUMBER_DATA;
    // H2:  packetSize 占位（稍后回填）
    qint64 packetSizePos = buffer.size();
    ds << quint32(0);
    // H3:  version
    ds << DATA_VERSION;
    // H4:  packetId
    ds << quint32(0);
    // H5:  unixTimeUtc
    ds << static_cast<quint32>(QDateTime::currentSecsSinceEpoch());

    if(objects.isEmpty())
    {
        ds << quint8(0);  // hasUser = 0
        ds << quint16(0); // numAi = 0
    }
    else
    {
        // U0: hasUser = 1（第一条 DCS 记录为玩家自身）
        ds << quint8(1);

        const DcsObject &user = objects.first();
        writeAircraftBase(ds, 0, user,
                          dcsIdToCategory(user.id),
                          dcsIdToEngineType(user.id),
                          AFLAG_IS_USER, true);
        writeUserAircraftExtra(ds);

        // AI / 其他对象
        quint16 numAi = static_cast<quint16>(qMin(objects.size() - 1, 65535));
        ds << numAi;
        for(int i = 1; i < objects.size() && (i - 1) < 65535; i++)
        {
            const DcsObject &ai = objects[i];
            writeAircraftBase(ds, static_cast<quint32>(i), ai,
                              dcsIdToCategory(ai.id),
                              dcsIdToEngineType(ai.id),
                              0, false);
        }
    }

    // M0: numMetar = 0（Little Xpconnect 不写气象）
    ds << quint16(0);

    // 回填 packetSize = 剩余字节数（不含 magic 4B + packetSize 自身 4B）
    quint32 packetSize = static_cast<quint32>(buffer.size() - packetSizePos - sizeof(quint32));
    QDataStream patchDs(&buffer, QIODevice::WriteOnly);
    patchDs.device()->seek(packetSizePos);
    patchDs << packetSize;

    return buffer;
}

// 写入共享内存
static void writeSharedMemory(QSharedMemory &shm, const QByteArray &payload)
{
    quint32 totalSize = static_cast<quint32>(8 + payload.size());
    if(totalSize > SHARED_MEMORY_SIZE)
    {
        qWarning() << "Data too large for shared memory:" << totalSize << ">" << SHARED_MEMORY_SIZE;
        return;
    }

    QByteArray shmData(SHARED_MEMORY_SIZE, 0);
    QDataStream ds(&shmData, QIODevice::WriteOnly);
    // 共享内存头部仅写两个 quint32，无需 setVersion
    ds << totalSize;   // 偏移 0: 有效字节总数
    ds << quint32(0);  // 偏移 4: terminate 标志
    ds.writeRawData(payload.constData(), payload.size());

    if(shm.lock())
    {
        memcpy(shm.data(), shmData.constData(), SHARED_MEMORY_SIZE);
        shm.unlock();
    }
    else
    {
        qWarning() << "Cannot lock shared memory:" << shm.errorString();
    }
}

// ============ 主桥接类 ============

class DcsBridge : public QObject
{
    Q_OBJECT

public:
    explicit DcsBridge(QObject *parent = nullptr)
        : QObject(parent)
    {
        // ---- 初始化 UDP socket ----
        m_socket = new QUdpSocket(this);
        if(!m_socket->bind(QHostAddress::LocalHost, UDP_PORT))
        {
            qCritical() << "Failed to bind UDP port" << UDP_PORT << ":" << m_socket->errorString();
            QCoreApplication::exit(1);
            return;
        }
        connect(m_socket, &QUdpSocket::readyRead, this, &DcsBridge::onReadyRead);

        // ---- 初始化共享内存 ----
        m_shm = new QSharedMemory(QLatin1String(SHARED_MEMORY_KEY), this);
        if(!m_shm->create(SHARED_MEMORY_SIZE, QSharedMemory::ReadWrite))
        {
            if(m_shm->error() == QSharedMemory::AlreadyExists)
            {
                // 共享内存已存在（可能由 Little Xpconnect 或其他实例创建），附加即可
                if(!m_shm->attach(QSharedMemory::ReadWrite))
                {
                    qCritical() << "Failed to attach shared memory:" << m_shm->errorString();
                    QCoreApplication::exit(1);
                    return;
                }
                qDebug() << "Attached to existing shared memory" << SHARED_MEMORY_KEY;
            }
            else
            {
                qCritical() << "Failed to create shared memory:" << m_shm->errorString();
                QCoreApplication::exit(1);
                return;
            }
        }
        else
        {
            qDebug() << "Created shared memory" << SHARED_MEMORY_KEY << "size:" << SHARED_MEMORY_SIZE;
        }

        qDebug() << "DCS Bridge started, listening on UDP port" << UDP_PORT;
    }

    ~DcsBridge() override
    {
        // 写 terminate 标志通知 Little Navmap 断开
        if(m_shm && m_shm->isAttached())
        {
            QByteArray shmData(SHARED_MEMORY_SIZE, 0);
            QDataStream ds(&shmData, QIODevice::WriteOnly);
            // 共享内存头部仅写两个 quint32，无需 setVersion
            ds << quint32(8);  // totalSize = 8
            ds << quint32(1);  // terminate = 1
            if(m_shm->lock())
            {
                memcpy(m_shm->data(), shmData.constData(), SHARED_MEMORY_SIZE);
                m_shm->unlock();
            }
            m_shm->detach();
        }
        qDebug() << "DCS Bridge stopped";
    }

private slots:
    void onReadyRead()
    {
        while(m_socket->hasPendingDatagrams())
        {
            QByteArray datagram;
            datagram.resize(static_cast<int>(m_socket->pendingDatagramSize()));
            m_socket->readDatagram(datagram.data(), datagram.size());

            QList<DcsObject> objects = parseDatagram(datagram);
            if(objects.isEmpty())
                continue;

            QByteArray payload = buildPayload(objects);
            writeSharedMemory(*m_shm, payload);
        }
    }

private:
    // 解析 DCS Export.lua 发送的 UDP 数据包
    static QList<DcsObject> parseDatagram(const QByteArray &data)
    {
        QList<DcsObject> result;

        if(data.size() < 8)
        {
            qWarning() << "Datagram too small:" << data.size();
            return result;
        }

        QDataStream ds(data);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds.setFloatingPointPrecision(QDataStream::DoublePrecision);

        quint32 header;
        qint32  count;
        ds >> header >> count;

        if(header != DCS_HEADER)
        {
            qWarning() << "Invalid DCS header:" << Qt::hex << header;
            return result;
        }

        if(count < 0 || count > 1000)
        {
            qWarning() << "Invalid object count:" << count;
            return result;
        }

        // 每条记录: id(4) + name(8) + alt(4) + heading(4) + lat(8) + lon(8) = 36 bytes
        int expectedSize = 8 + count * 36;
        if(data.size() < expectedSize)
        {
            qWarning() << "Datagram truncated: expected" << expectedSize << "got" << data.size();
            return result;
        }

        for(int i = 0; i < count; i++)
        {
            DcsObject obj;
            quint32 id;
            qint32  altFt, headingDeg;
            char    nameBuf[9] = {};

            ds >> id;
            ds.readRawData(nameBuf, 8);
            ds >> altFt >> headingDeg;
            ds >> obj.latRad >> obj.lonRad;

            obj.id        = id;
            obj.name      = QString::fromUtf8(nameBuf, qstrnlen(nameBuf, 8)).trimmed();
            obj.altFt     = altFt;
            obj.headingDeg = headingDeg;

            result.append(obj);
        }

        return result;
    }

    QUdpSocket    *m_socket = nullptr;
    QSharedMemory *m_shm    = nullptr;
};

// ============ 入口 ============

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    qDebug() << "=== DCS → Little Navmap Shared Memory Bridge ===";
    DcsBridge bridge;
    return a.exec();
}

#include "main.moc"