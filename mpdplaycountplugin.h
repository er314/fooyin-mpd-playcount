#pragma once

#include <core/library/musiclibrary.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <core/library/librarymanager.h>

#include <QHash>
#include <QMap>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>

namespace Fooyin::MpdPlaycount {

class Plugin final : public QObject, public Fooyin::Plugin, public Fooyin::CorePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "mpdplaycount.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin)

public:
    void initialise(const Fooyin::CorePluginContext& context) override;
    void shutdown() override;

private:
    enum class PendingCommand
    {
        None,
        Status,
        CurrentSong
    };

    struct Session
    {
        bool active{false};
        bool counted{false};
        QString songId;
        QString relativeFile;
        QString state;
        double duration{0.0};
        double lastElapsed{0.0};
        double listened{0.0};
    };

    using Fields = QMap<QString, QString>;

    void pollMpd();
    void readSocket();
    void processLine(const QString& line);
    void sendCommand(const QByteArray& command, PendingCommand pending);
    void handleStatus(const Fields& status);
    void handleCurrentSong(const Fields& song);
    void updateSession(const Fields& status);
    void finishSession();
    bool countCurrentTrack();
    Fooyin::Track findLibraryTrack(const QString& relativeFile) const;

    static double seconds(const QString& value);
    static QString normalizedPath(const QString& path);

    Fooyin::MusicLibrary* m_library{nullptr};

    QString m_musicRoot;
    const double m_playThreshold{60.0};  // play count +1 after 60 seconds of playback
    const double m_endTolerance{6.0};    // note : polling interval is set to 5 seconds

    QTcpSocket m_socket;
    QTimer m_pollTimer;
    QByteArray m_receiveBuffer;
    Fields m_reply;
    Fields m_pendingStatus;
    Fields m_currentSongFields;
    PendingCommand m_pending{PendingCommand::None};
    bool m_greeted{false};

    Session m_session;
    QHash<int, int> m_writtenCounts;
};

} // namespace Fooyin::MpdPlaycount
