#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace radiosplit {

class SideOutput {
public:
    SideOutput();
    ~SideOutput();

    SideOutput(const SideOutput&) = delete;
    SideOutput& operator=(const SideOutput&) = delete;

    bool Init();
    const std::vector<std::string>& PlaybackNames() const { return names_; }
    const std::vector<char>& PlaybackDefaults() const { return defaults_; }

    // deviceIndex < 0 opens the Windows default playback device.
    bool Start(int deviceIndex, int sampleRate, int channels);
    void Stop();
    bool Running() const { return running_; }
    const std::string& OpenedName() const { return openedName_; }

    void SetVolumeHundredths(long volumeHundredths);
    void SetLinearGain(float gain);
    void Push(const void* src, std::size_t bytes);
    void Clear();

private:
    struct Ma;
    Ma* ma_ = nullptr;
    std::vector<std::string> names_;
    std::vector<char> defaults_;
    std::string openedName_;
    bool running_ = false;
    int sampleRate_ = 0;
    int channels_ = 0;
};

}  // namespace radiosplit
