#include "mpdplaycountplugin.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>

#include <algorithm>

namespace Fooyin::MpdPlaycount {

void Plugin::initialise(const Fooyin::CorePluginContext& context)
{
    m_library = context.library;

    const auto& libraries = context.libraryManager->allLibraries();
    if (libraries.empty()) {
        qWarning() << "No Fooyin library paths configured; MPD plays won't be recorded";
        return;
    }
    m_musicRoot = libraries.begin()->second.path;  // Retrieve the 1st library path configured

    connect(&m_socket, &QTcpSocket::readyRead, this, [this] {
        readSocket();
    });

    connect(&m_socket, &QTcpSocket::disconnected, this, [this] {
        m_greeted = false;
        m_pending = PendingCommand::None;
        m_reply.clear();
        m_receiveBuffer.clear();

        // Don't count time while disconnected; the next status starts a new interval.
        m_session.state.clear();
    });

    connect(&m_socket, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                qWarning() << "MPD connection error:" << m_socket.errorString();
            });

    connect(&m_pollTimer, &QTimer::timeout, this, [this] {
        pollMpd();
    });

    m_pollTimer.setInterval(5000);  // Poll MPD every 5 seconds
    m_pollTimer.start();
    pollMpd();
}

void Plugin::shutdown()
{
    m_pollTimer.stop();
    m_socket.abort();
    m_session.active = false;
}

void Plugin::pollMpd()
{
    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        m_socket.connectToHost(QHostAddress::LocalHost, 6600);
        return;
    }

    if (!m_greeted || m_pending != PendingCommand::None) {
        return;
    }

    sendCommand("status", PendingCommand::Status);
}

void Plugin::sendCommand(const QByteArray& command, PendingCommand pending)
{
    if (m_socket.state() != QAbstractSocket::ConnectedState) {
        return;
    }

    m_reply.clear();
    m_pending = pending;
    m_socket.write(command);
    m_socket.write("\n");
}

void Plugin::readSocket()
{
    m_receiveBuffer += m_socket.readAll();

    while (true) {
        const qsizetype newline = m_receiveBuffer.indexOf('\n');
        if (newline < 0) {
            return;
        }

        const QByteArray rawLine = m_receiveBuffer.left(newline);
        m_receiveBuffer.remove(0, newline + 1);

        processLine(QString::fromUtf8(rawLine).trimmed());
    }
}

void Plugin::processLine(const QString& line)
{
    if (!m_greeted) {
        if (line.startsWith(QStringLiteral("OK MPD "))) {
            m_greeted = true;
        } else if (!line.isEmpty()) {
            qWarning() << "Unexpected MPD greeting:" << line;
        }
        return;
    }

    if (line == QStringLiteral("OK")) {
        const PendingCommand completed = m_pending;
        const Fields fields = m_reply;
        m_pending = PendingCommand::None;
        m_reply.clear();

        if (completed == PendingCommand::Status) {
            handleStatus(fields);
        } else if (completed == PendingCommand::CurrentSong) {
            handleCurrentSong(fields);
        }
        return;
    }

    if (line.startsWith(QStringLiteral("ACK"))) {
        qWarning() << "MPD command failed:" << line;
        m_pending = PendingCommand::None;
        m_reply.clear();
        return;
    }

    const qsizetype separator = line.indexOf(QStringLiteral(": "));
    if (separator > 0) {
        m_reply.insert(line.left(separator), line.mid(separator + 2));
    }
}

void Plugin::handleStatus(const Fields& status)
{
    const QString songId = status.value(QStringLiteral("songid"));

    if (songId.isEmpty()) {
        if (m_session.active) {
            finishSession();
            m_session.active = false;
        }
        return;
    }

    if (!m_session.active || songId != m_session.songId) {
        if (m_session.active) {
            finishSession();
        }

        m_pendingStatus = status;
        sendCommand("currentsong", PendingCommand::CurrentSong);
        return;
    }

    updateSession(status);
}

void Plugin::handleCurrentSong(const Fields& song)
{
    const QString expectedId = m_pendingStatus.value(QStringLiteral("songid"));
    const QString returnedId = song.value(QStringLiteral("Id"));
    const QString file = song.value(QStringLiteral("file"));

    // The track may have changed again between status and currentsong.
    if (expectedId.isEmpty() || returnedId != expectedId || file.isEmpty()) {
        return;
    }

    m_currentSongFields = song;

    m_session = {};
    m_session.active = true;
    m_session.songId = expectedId;
    m_session.relativeFile = file;
    m_session.state = m_pendingStatus.value(QStringLiteral("state"));
    m_session.lastElapsed = seconds(m_pendingStatus.value(QStringLiteral("elapsed")));
    m_session.duration = seconds(m_pendingStatus.value(QStringLiteral("duration")));

    if (m_session.duration <= 0.0) {
        m_session.duration = seconds(song.value(QStringLiteral("duration")));
    }
    if (m_session.duration <= 0.0) {
        m_session.duration = seconds(song.value(QStringLiteral("Time")));
    }

    qInfo() << "MPD track observed:" << file
            << "duration:" << m_session.duration;
}

void Plugin::updateSession(const Fields& status)
{
    const QString state = status.value(QStringLiteral("state"));
    const double elapsed = seconds(status.value(QStringLiteral("elapsed")));
    const double reportedDuration = seconds(status.value(QStringLiteral("duration")));
    const bool wasPlaying = (m_session.state == QStringLiteral("play"));

    if (reportedDuration > 0.0) {
        m_session.duration = reportedDuration;
    }

    // Accumulate only observed playback, not paused time or seeks.
    if (wasPlaying) {
        const double delta = elapsed - m_session.lastElapsed;

        if (delta > 0.0 && delta <= 6.5) {
            m_session.listened += delta;
        } else if (delta < -2.0 && elapsed < 6.5) {
            // A restart/seek to the beginning starts a new play attempt.
            m_session.listened = 0.0;
            m_session.counted = false;
        }
    }

    m_session.lastElapsed = elapsed;
    m_session.state = state;

    if (!m_session.counted
        && m_session.duration >= m_playThreshold
        && m_session.listened >= m_playThreshold) {
        m_session.counted = countCurrentTrack();
    }

    if (state == QStringLiteral("stop") && wasPlaying) {
        finishSession();
    }
}

void Plugin::finishSession()
{
    if (!m_session.active || m_session.counted) {
        return;
    }

    // For tracks shorter than one minute, count only when observed listening
    // time and the final MPD position indicate playback reached the end.
    if (m_session.duration > 0.0 && m_session.duration < m_playThreshold
        && m_session.listened + m_endTolerance >= m_session.duration
        && m_session.lastElapsed + m_endTolerance >= m_session.duration) {
        m_session.counted = countCurrentTrack();
    }
}

bool Plugin::countCurrentTrack()
{
    if (!m_library) {
        return false;
    }

    const Fooyin::Track track = findLibraryTrack(m_session.relativeFile);
    if (!track.isValid()) {
        qWarning() << "No unique Fooyin library track matched MPD file:"
                   << m_session.relativeFile;
        return false;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int oldCount = std::max(
        track.playCount(),
        m_writtenCounts.value(track.id(), track.playCount())
    );

    Fooyin::Track updated = track;
    updated.setPlayCount(oldCount + 1);

    if (updated.firstPlayed() == 0) {
        updated.setFirstPlayed(static_cast<uint64_t>(now));
    }
    updated.setLastPlayed(static_cast<uint64_t>(now));

    m_library->updateTrackStats(
        updated,
        Fooyin::Track::Stats{Fooyin::Track::Stat::Playcount}
    );

    m_writtenCounts.insert(track.id(), oldCount + 1);

    qInfo() << "Recorded MPD play:" << track.filepath()
            << "playcount:" << oldCount + 1;
    return true;
}

Fooyin::Track Plugin::findLibraryTrack(const QString& relativeFile) const
{
    if (!m_library) {
        return {};
    }

    const QString root = normalizedPath(m_musicRoot);
    const QString candidate = normalizedPath(QDir(m_musicRoot).filePath(relativeFile));

    if (candidate.isEmpty() || root.isEmpty()) {
        return {};
    }

    const QString rootPrefix = root.endsWith(QDir::separator())
        ? root
        : root + QDir::separator();

    if (candidate != root && !candidate.startsWith(rootPrefix)) {
        qWarning() << "MPD path escapes music root:" << relativeFile;
        return {};
    }

    QList<Fooyin::Track> matches;

    for (const Fooyin::Track& track : m_library->libraryTracks()) {
        if (normalizedPath(track.filepath()) == candidate) {
            matches.push_back(track);
        }
    }

    if (matches.size() == 1) {
        return matches.front();
    }

    // A CUE-backed file can have multiple Fooyin tracks at one physical path.
    // Use the MPD title to disambiguate; otherwise fail closed.
    const QString mpdTitle = m_currentSongFields.value(QStringLiteral("Title"));
    if (!mpdTitle.isEmpty()) {
        QList<Fooyin::Track> titleMatches;
        for (const Fooyin::Track& track : matches) {
            if (track.title().compare(mpdTitle, Qt::CaseInsensitive) == 0) {
                titleMatches.push_back(track);
            }
        }
        if (titleMatches.size() == 1) {
            return titleMatches.front();
        }
    }

    return {};
}

double Plugin::seconds(const QString& value)
{
    bool ok = false;
    const double result = value.toDouble(&ok);
    return ok ? result : 0.0;
}

QString Plugin::normalizedPath(const QString& path)
{
    if (path.isEmpty()) {
        return {};
    }

    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

} // namespace Fooyin::MpdPlaycount
