/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Vibrator.h"

#include <fcntl.h>
#include <log/log.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef LIVETAP_DEFAULT_F0
#define LIVETAP_DEFAULT_F0 170
#endif

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

namespace {

constexpr uint8_t kMaxAmplitude = 0xff;
constexpr int32_t kMinLevel = 800;
constexpr int32_t kMaxLevel = 2400;
constexpr int32_t kLevelStep = 100;
constexpr uint32_t kDoubleClickGapMs = 100;
constexpr int32_t kCompositionDelayMaxMs = 1000;

constexpr float kLightScale = 0.60f;
constexpr float kMediumScale = 0.80f;
constexpr float kStrongScale = 1.00f;

const std::vector<std::string> kDurationPaths = {
    "/sys/class/leds/vibrator/duration",
    "/sys/class/leds/vibrator/oplus_duration",
};

const std::vector<std::string> kActivatePaths = {
    "/sys/class/leds/vibrator/activate",
    "/sys/class/leds/vibrator/oplus_activate",
};

const std::vector<std::string> kVmaxPaths = {
    "/sys/class/leds/vibrator/vmax",
};

struct EffectProfile {
    uint32_t durationMs;
    uint8_t amplitude;
};

uint8_t scaleAmplitude(uint8_t amplitude, float scale) {
    int32_t value = static_cast<int32_t>(amplitude * scale + 0.5f);
    if (value < 1) value = 1;
    if (value > kMaxAmplitude) value = kMaxAmplitude;
    return static_cast<uint8_t>(value);
}

int32_t amplitudeToLevel(uint8_t amplitude) {
    if (amplitude == 0) return 0;
    int32_t steps = (kMaxLevel - kMinLevel) / kLevelStep;
    int32_t step = static_cast<int32_t>(
            std::round((static_cast<float>(amplitude) / kMaxAmplitude) * steps));
    return kMinLevel + step * kLevelStep;
}

bool isSupportedEffect(Effect effect) {
    switch (effect) {
        case Effect::CLICK:
        case Effect::DOUBLE_CLICK:
        case Effect::TICK:
        case Effect::THUD:
        case Effect::POP:
        case Effect::HEAVY_CLICK:
        case Effect::TEXTURE_TICK:
            return true;
        default:
            return false;
    }
}

float getStrengthScale(EffectStrength strength) {
    switch (strength) {
        case EffectStrength::LIGHT:
            return kLightScale;
        case EffectStrength::MEDIUM:
            return kMediumScale;
        case EffectStrength::STRONG:
            return kStrongScale;
        default:
            return -1.0f;
    }
}

EffectProfile getEffectProfile(Effect effect) {
    switch (effect) {
        case Effect::TEXTURE_TICK:
            return {10, 110};
        case Effect::TICK:
            return {15, 160};
        case Effect::CLICK:
        case Effect::DOUBLE_CLICK:
            return {20, 190};
        case Effect::POP:
            return {25, 210};
        case Effect::THUD:
            return {30, 230};
        case Effect::HEAVY_CLICK:
            return {35, 255};
        default:
            return {20, 190};
    }
}

EffectProfile getPrimitiveProfile(CompositePrimitive primitive) {
    switch (primitive) {
        case CompositePrimitive::CLICK:
        case CompositePrimitive::QUICK_RISE:
            return {20, 190};
        case CompositePrimitive::THUD:
        case CompositePrimitive::SLOW_RISE:
        case CompositePrimitive::SPIN:
            return {30, 230};
        case CompositePrimitive::LIGHT_TICK:
            return {12, 140};
        case CompositePrimitive::LOW_TICK:
            return {12, 160};
        case CompositePrimitive::QUICK_FALL:
            return {12, 140};
        default:
            return {0, 0};
    }
}

}  // namespace

Vibrator::Vibrator() {
    mDurationPath = lookupPath(kDurationPaths);
    mActivatePath = lookupPath(kActivatePaths);
    mVmaxPath = lookupPath(kVmaxPaths);

    if (mDurationPath.empty() || mActivatePath.empty()) {
        ALOGE("LiveTap init failed: sysfs interface not found");
        return;
    }

    mF0 = LIVETAP_DEFAULT_F0;
    FILE* fp = fopen("/sys/class/leds/vibrator/f0", "r");
    if (fp != nullptr) {
        int val = 0;
        if (fscanf(fp, "%d", &val) == 1 && val >= 100 && val <= 400) {
            mF0 = val;
        }
        fclose(fp);
    }

    if (!mVmaxPath.empty()) {
        writeValue(mVmaxPath, kMaxLevel);
    }

    mReady = true;
    ALOGI("LiveTap init success: f0 %d", mF0);
}

std::string Vibrator::lookupPath(const std::vector<std::string>& candidates) {
    std::string firstExisting;

    for (const auto& path : candidates) {
        if (access(path.c_str(), F_OK) != 0) {
            continue;
        }

        if (firstExisting.empty()) {
            firstExisting = path;
        }

        if (access(path.c_str(), W_OK) == 0) {
            return path;
        }
    }

    return firstExisting;
}

bool Vibrator::writeValue(const std::string& path, int32_t value) {
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        ALOGE("LiveTap: failed to open %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", value);
    ssize_t ret = write(fd, buf, len);
    close(fd);

    if (ret != len) {
        ALOGE("LiveTap: failed to write %d to %s: %s", value, path.c_str(),
              ret < 0 ? strerror(errno) : "short write");
        return false;
    }

    return true;
}

int32_t Vibrator::playEffect(uint32_t durationMs, uint8_t amplitude) {
    if (!mVmaxPath.empty()) {
        writeValue(mVmaxPath, amplitudeToLevel(amplitude));
    }

    if (!writeValue(mDurationPath, static_cast<int32_t>(durationMs)) ||
        !writeValue(mActivatePath, 1)) {
        return -1;
    }

    ALOGI("LiveTap play: duration %u ms, amplitude %u", durationMs, amplitude);
    return static_cast<int32_t>(durationMs);
}

void Vibrator::playComposition(std::vector<CompositeEffect> composite,
                               const std::shared_ptr<IVibratorCallback>& callback) {
    struct Step {
        int32_t delayMs;
        uint32_t durationMs;
        uint8_t amplitude;
    };

    std::vector<Step> steps;
    int32_t pendingDelayMs = 0;

    for (const auto& effect : composite) {
        pendingDelayMs += effect.delayMs;

        if (effect.primitive == CompositePrimitive::NOOP || effect.scale <= 0.0f) {
            continue;
        }

        EffectProfile profile = getPrimitiveProfile(effect.primitive);
        if (profile.durationMs == 0) {
            continue;
        }

        uint8_t amp = scaleAmplitude(profile.amplitude, effect.scale);

        if (!steps.empty() && pendingDelayMs == 0 &&
            std::abs(static_cast<int>(steps.back().amplitude) - static_cast<int>(amp)) <= 5) {
            steps.back().durationMs += profile.durationMs;
        } else {
            steps.push_back({pendingDelayMs, profile.durationMs, amp});
            pendingDelayMs = 0;
        }
    }

    if (steps.empty()) {
        if (callback != nullptr) {
            callback->onComplete();
        }
        return;
    }

    uint32_t generation = mGeneration.load();

    std::thread([this, steps = std::move(steps), callback, generation] {
        for (const auto& step : steps) {
            if (mGeneration.load() != generation) {
                return;
            }

            if (step.delayMs > 0) {
                usleep(step.delayMs * 1000);
                if (mGeneration.load() != generation) {
                    return;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mGeneration.load() != generation) {
                    return;
                }
                playEffect(step.durationMs, step.amplitude);
            }

            usleep(step.durationMs * 1000);
        }

        if (mGeneration.load() == generation && callback != nullptr) {
            callback->onComplete();
        }
    }).detach();
}

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = static_cast<int32_t>(IVibrator::CAP_ON_CALLBACK) |
                    static_cast<int32_t>(IVibrator::CAP_PERFORM_CALLBACK) |
                    static_cast<int32_t>(IVibrator::CAP_AMPLITUDE_CONTROL) |
                    static_cast<int32_t>(IVibrator::CAP_COMPOSE_EFFECTS);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    if (!mReady) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    mGeneration.fetch_add(1);

    std::lock_guard<std::mutex> lock(mMutex);
    mAmplitudeSet = false;

    if (!writeValue(mActivatePath, 0)) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    if (!mReady) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (timeoutMs <= 0) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    mGeneration.fetch_add(1);

    int32_t duration;
    {
        std::lock_guard<std::mutex> lock(mMutex);

        if (!mAmplitudeSet && !mVmaxPath.empty()) {
            writeValue(mVmaxPath, kMaxLevel);
        }
        mAmplitudeSet = false;

        if (!writeValue(mDurationPath, timeoutMs) || !writeValue(mActivatePath, 1)) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
        }

        duration = timeoutMs;
    }

    if (callback != nullptr) {
        std::thread([callback, duration] {
            usleep(duration * 1000);
            callback->onComplete();
        }).detach();
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength es,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    if (!mReady) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (!isSupportedEffect(effect)) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    float strengthScale = getStrengthScale(es);
    if (strengthScale < 0.0f) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    mGeneration.fetch_add(1);

    if (effect == Effect::DOUBLE_CLICK) {
        std::vector<CompositeEffect> composite(2);
        composite[0].primitive = CompositePrimitive::CLICK;
        composite[0].scale = strengthScale;
        composite[0].delayMs = 0;
        composite[1].primitive = CompositePrimitive::CLICK;
        composite[1].scale = strengthScale;
        composite[1].delayMs = kDoubleClickGapMs;

        playComposition(composite, callback);

        EffectProfile profile = getEffectProfile(Effect::CLICK);
        *_aidl_return = static_cast<int32_t>(profile.durationMs * 2 + kDoubleClickGapMs);
        return ndk::ScopedAStatus::ok();
    }

    int32_t duration;
    {
        std::lock_guard<std::mutex> lock(mMutex);

        EffectProfile profile = getEffectProfile(effect);
        uint8_t amplitude = scaleAmplitude(profile.amplitude, strengthScale);

        int32_t played = playEffect(profile.durationMs, amplitude);
        if (played < 0) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
        }

        duration = played;
    }

    if (callback != nullptr) {
        std::thread([callback, duration] {
            usleep(duration * 1000);
            callback->onComplete();
        }).detach();
    }

    *_aidl_return = duration;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    *_aidl_return = {Effect::CLICK,        Effect::DOUBLE_CLICK, Effect::TICK,
                     Effect::THUD,         Effect::POP,          Effect::HEAVY_CLICK,
                     Effect::TEXTURE_TICK};

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    if (!mReady) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (amplitude <= 0.0f || amplitude > 1.0f) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    int32_t steps = (kMaxLevel - kMinLevel) / kLevelStep;
    int32_t step = static_cast<int32_t>(std::round(amplitude * steps));
    int32_t level = kMinLevel + step * kLevelStep;

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mVmaxPath.empty() && !writeValue(mVmaxPath, level)) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    mAmplitudeSet = true;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool /* enabled */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* maxDelayMs) {
    if (maxDelayMs == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
    *maxDelayMs = kCompositionDelayMaxMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* maxSize) {
    if (maxSize == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
    *maxSize = 256;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(
        std::vector<CompositePrimitive>* supported) {
    *supported = {
        CompositePrimitive::NOOP,
        CompositePrimitive::CLICK,
        CompositePrimitive::THUD,
        CompositePrimitive::SPIN,
        CompositePrimitive::QUICK_RISE,
        CompositePrimitive::SLOW_RISE,
        CompositePrimitive::QUICK_FALL,
        CompositePrimitive::LIGHT_TICK,
        CompositePrimitive::LOW_TICK,
    };
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive,
                                                  int32_t* durationMs) {
    if (durationMs == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    EffectProfile profile = getPrimitiveProfile(primitive);
    if (primitive != CompositePrimitive::NOOP && profile.durationMs == 0) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    *durationMs = static_cast<int32_t>(profile.durationMs);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    if (composite.empty() || composite.size() > 256) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    if (!mReady) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    for (const auto& effect : composite) {
        if (effect.delayMs < 0 || effect.delayMs > kCompositionDelayMaxMs || effect.scale < 0.0f ||
            effect.scale > 1.0f) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
        }
    }

    mGeneration.fetch_add(1);
    playComposition(composite, callback);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(
        std::vector<Effect>* /* _aidl_return */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t /* id */, Effect /* effect */,
                                            EffectStrength /* strength */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t /* id */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* resonantFreqHz) {
    if (resonantFreqHz == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
    *resonantFreqHz = static_cast<float>(mF0 > 0 ? mF0 : LIVETAP_DEFAULT_F0);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getQFactor(float* qFactor) {
    if (qFactor == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
    *qFactor = 10.0f;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float* /* freqResolutionHz */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float* /* freqMinimumHz */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>* /* _aidl_return */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t* /* durationMs */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t* /* maxSize */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>* /* supported */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>& /* composite */,
                                         const std::shared_ptr<IVibratorCallback>& /* callback */) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
