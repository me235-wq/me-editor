#pragma once
#include "core/Project.h"
#include <QObject>

namespace ve {

// Live audio preview. Runs the AudioMixer on its own thread and feeds the sound card, so volume, fades, mute,
// speed, keyframes and track mute are heard exactly as they will be exported. (Built only when Qt Multimedia is present.)
class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer() override;
    bool available() const { return m_available; }     // false: no sound output device (or built without Qt Multimedia)
    void play(const ProjectData& snapshot, double fromSeconds);
    void updateProject(const ProjectData& snapshot);    // call after edits while playing
    void stop();
    void beep();    // plays a one second test tone through the same path (File → Test audio output)

signals:
    void message(const QString& text);   // which device is used / what went wrong (shown in the status bar)

private:
    class Worker;
    Worker* m_worker = nullptr;
    class QThread* m_thread = nullptr;
    bool m_available = false;
};

} // namespace ve
