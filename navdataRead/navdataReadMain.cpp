/*****************************************************************************
 * 从伪 X-Plane 12 根目录编译 Little Navmap 离线库 little_navmap_xp12.sqlite。
 * 依赖嵌入在 embed_atools/ 的 atools 源码（GPL-3.0），未将 atools 作为 CMake 子目标。
 *
 * 用法：
 *   dataRead <伪XP根目录> [输出的.sqlite路径]
 * 若省略参数，则使用下方 kDefaultPseudoXpRoot，并在当前工作目录写入 little_navmap_xp12.sqlite。
 *****************************************************************************/

#include "exception.h"
#include "fs/navdatabase.h"
#include "fs/navdatabaseerrors.h"
#include "fs/navdatabaseoptions.h"
#include "sql/sqldatabase.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

/* 默认伪 X-Plane 根目录：须包含 Custom Data 或 Resources/default data 下的 earth_fix.dat / earth_awy.dat / earth_nav.dat 等（见项目内 数据库.md） */
#if defined(Q_OS_WIN)
const fs::path kDefaultPseudoXpRoot(R"(C:\path\to\pseudo_xplane12)");
#else
const fs::path kDefaultPseudoXpRoot("/path/to/pseudo_xplane12");
#endif

constexpr auto kConnectionName = "LittleNavmapDCS_dataRead";
constexpr auto kSqliteDriver = "QSQLITE";

QString pathToQString(const fs::path& p)
{
#if defined(Q_OS_WIN)
  const std::wstring w = p.wstring();
  return QString::fromWCharArray(w.data(), static_cast<int>(w.size()));
#else
  const std::u8string u8 = p.u8string();
  return QString::fromUtf8(reinterpret_cast<const char *>(u8.data()), static_cast<int>(u8.size()));
#endif
}

fs::path pathFromArg(char *arg)
{
#if defined(Q_OS_WIN)
  return fs::path(QString::fromLocal8Bit(arg).trimmed().toStdWString());
#else
  const QByteArray utf8 = QString::fromUtf8(arg).trimmed().toUtf8();
  return fs::path(std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
#endif
}

/** 含 sqldrivers 的 plugins 父目录列表（Qt 安装路径 + exe 旁 deploy）。 */
static QStringList sqlDriverPluginRoots(int argc, char *argv[])
{
  QStringList roots;

  const QString qtPlugins = QLibraryInfo::path(QLibraryInfo::PluginsPath);
  if(!qtPlugins.isEmpty() && QDir(qtPlugins + QStringLiteral("/sqldrivers")).exists())
    roots.append(qtPlugins);

  if(argc > 0 && argv[0] != nullptr)
  {
    const QString appDir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    const QString deployPlugins = appDir + QStringLiteral("/plugins");
    if(QDir(deployPlugins + QStringLiteral("/sqldrivers")).exists())
      roots.append(deployPlugins);
  }

  return roots;
}

static void mergeQtPluginPathEnv(const QStringList& roots)
{
  if(roots.isEmpty())
    return;

  const QChar sep =
#if defined(Q_OS_WIN)
      QLatin1Char(';');
#else
      QLatin1Char(':');
#endif
  QString merged = roots.join(sep);
  const QByteArray existing = qgetenv("QT_PLUGIN_PATH");
  if(!existing.isEmpty())
    merged = QString::fromLocal8Bit(existing) + sep + merged;
  qputenv("QT_PLUGIN_PATH", merged.toLocal8Bit());
}

} // namespace

int main(int argc, char *argv[])
{
  /* 须在 QCoreApplication 构造之前设置，否则部分构建下 SQL 驱动不会被扫描到 */
  const QStringList sqlPluginRoots = sqlDriverPluginRoots(argc, argv);
  mergeQtPluginPathEnv(sqlPluginRoots);

  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("dataRead"));
  QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

  for(const QString& p : sqlPluginRoots)
    QCoreApplication::addLibraryPath(p);

  if(sqlPluginRoots.isEmpty())
    qWarning("dataRead: 未找到 plugins/sqldrivers，无法加载 QSQLITE。请设置 QT_PLUGIN_PATH 指向 Qt 的 plugins，"
             "或将 qsqlsqlite.dll 置于 <exe>/plugins/sqldrivers/。");

  fs::path xplaneFolder = R"(G:\SteamLibrary\steamapps\common\X-Plane 12\)";
  fs::path databasePath = fs::current_path() / "little_navmap_xp12.sqlite";

  if(argc >= 2)
    xplaneFolder = pathFromArg(argv[1]);
  if(argc >= 3)
    databasePath = pathFromArg(argv[2]);

  if(argc >= 2 && xplaneFolder.empty())
  {
    std::cerr << "Empty X-Plane root path.\n";
    return 1;
  }

  std::error_code ec;
  if(!fs::is_directory(xplaneFolder, ec))
  {
    std::cerr << "Not a directory (pseudo X-Plane root): " << xplaneFolder << "\n";
    std::cerr << "Usage: " << argv[0] << " <pseudo_xp_root> [output.sqlite]\n";
    return 1;
  }

  const fs::path xplaneFolderAbs = fs::weakly_canonical(xplaneFolder, ec);
  const QString xplaneFolderQt = pathToQString(xplaneFolderAbs.empty() ? xplaneFolder : xplaneFolderAbs);
  const QString databasePathQt = pathToQString(databasePath.lexically_normal());

  QFile::remove(databasePathQt);

  atools::sql::SqlDatabase::addDatabase(QString::fromLatin1(kSqliteDriver), QString::fromLatin1(kConnectionName));
  atools::sql::SqlDatabase db(QString::fromLatin1(kConnectionName));
  db.setDatabaseName(databasePathQt);

  try
  {
    db.open();
  }
  catch(atools::Exception& e)
  {
    std::cerr << "Cannot open database: " << e.what() << "\n";
    atools::sql::SqlDatabase::removeDatabase(QString::fromLatin1(kConnectionName));
    return 1;
  }

  atools::fs::NavDatabaseOptions opts;
  opts.setSimulatorType(atools::fs::FsPaths::XPLANE_12);
  opts.setBasepath(xplaneFolderQt);
  opts.setCallDefaultCallback(true);
  /* 默认构造的 opts 不会加载 ini：ResolveRoutes 等为 false，必须开启才会把 tmp_airway_point 解析写入 airway 表 */
  opts.setResolveAirways(true);

  atools::fs::NavDatabaseErrors errors;
  atools::fs::NavDatabase nav(opts, db, &errors, QStringLiteral("LittleNavmapDCS_connector"));

  try
  {
    nav.compileDatabase();
  }
  catch(atools::Exception& e)
  {
    std::cerr << e.what() << "\n";
    db.close();
    atools::sql::SqlDatabase::removeDatabase(QString::fromLatin1(kConnectionName));
    return 1;
  }

  db.close();
  atools::sql::SqlDatabase::removeDatabase(QString::fromLatin1(kConnectionName));

  std::cout << "Wrote " << databasePath << "\n";
  return 0;
}
