#include "Effects.h"
#include <algorithm>
#include <cmath>

namespace plasticz {
namespace {
constexpr float bias = 1.000000045813705e-18f;
// The legacy constructor fixes +0x5cc before subsequent host rate updates.
constexpr float constructorReciprocalRate = 1.0f / 44100.0f;
constexpr float divisions[] = {0.25f, 0.5f, 1.0f, 2.0f};
constexpr float patterns[32] = {
    1,0,1,0,1,0,1,0, 1,0,1,0,1,1,1,0,
    1,0,0,1,0,0,1,0, 1,0,1,1,0,1,1,0
};
float fraction(float value) noexcept { return value - static_cast<float>(static_cast<int>(value)); }
float triangle(float phase) noexcept {
    float value = fraction(phase) - 0.5f;
    value = value + value;
    value = std::abs(value) - 0.5f;
    return value + value;
}
float sineApprox(float phase) noexcept {
    float x = fraction(phase) * 6.2831854820251465f - 3.1415927410125732f;
    const float square = x * x;
    float poly = square * -2.3900000201138027e-08f + 2.7526000394573202e-06f;
    poly = poly * square - 0.00019840900495182723f;
    poly = poly * square + 0.008333331905305386f;
    poly = poly * square - 0.1666666716337204f;
    poly = poly * square + 1.0f;
    return -(poly * x);
}
float parabolic(float phase) noexcept {
    float x = fraction(phase);
    if (x < 0.5f) return (x*x)*-16.0f + x*8.0f;
    x -= 0.5f;
    return (x*x)*16.0f + x*-8.0f;
}
}

void Effects::prepare(float sampleRate) {
    sampleRate_ = sampleRate;
    delay_.resize(4u * 0x40000u);
    reverb_.prepare(sampleRate);
    reset();
}

void Effects::reset() noexcept {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    state_.fill(0.0f);
    position_.fill(0);
    crusherCount_ = 0;
    reverb_.resetFresh();
    clearDelayOnTypeChange_ = false;
    type_ = static_cast<int>(parameters_[28] * 10.989999771118164f);
}

void Effects::resetEffectState() noexcept {
    // 0x12fcd..0x130a0. Preserve the original omitted third formant history.
    position_.fill(0);
    crusherCount_ = 0;
    for (unsigned a = 0x4005dc; a <= 0x40060c; a += 4) s(a) = 0.0f;
    for (unsigned a : {0x4006a4u,0x4006a8u,0x4006b0u,0x4006b4u,0x4006bcu,0x4006c0u}) s(a) = 0.0f;
    for (unsigned a = 0x4006c8; a <= 0x400724; a += 4) s(a) = 0.0f;
    if (clearDelayOnTypeChange_) {
        std::fill(delay_.begin(), delay_.end(), 0.0f);
        reverb_.reset();
        clearDelayOnTypeChange_ = false;
    }
}

void Effects::setParameters(const Parameters& values) noexcept {
    const int next = static_cast<int>(values[28] * 10.989999771118164f);
    if (next != type_) resetEffectState();
    type_ = next;
    parameters_ = values;
}

float Effects::interpolated(unsigned channel, float offset, unsigned extra) noexcept {
    const int integer = static_cast<int>(offset);
    const float part = offset - static_cast<float>(integer);
    const auto at = position_[0] + static_cast<std::uint32_t>(integer) + extra;
    return (1.0f - part) * tap(channel, at & 511u) + part * tap(channel, (at+1u) & 511u);
}

float Effects::quadrature(float input, float phase) noexcept {
    // Two three-stage allpass branches; original helper 0x10bbc.
    const float a = (s(0x4006e0)-input)*0.2328290045261383f+s(0x4006d8);
    const float b = (s(0x4006e8)-a)*0.8435729742050171f+s(0x4006e0);
    const float c = (s(0x4006f0)-b)*0.9803509712219238f+s(0x4006e8);
    s(0x4006c8)=a; s(0x4006cc)=b; s(0x4006d0)=c;
    s(0x4006f0)=s(0x4006ec); s(0x4006ec)=c;
    s(0x4006e8)=s(0x4006e4); s(0x4006e4)=b;
    s(0x4006e0)=s(0x4006dc); s(0x4006dc)=a;
    s(0x4006d8)=s(0x4006d4); s(0x4006d4)=input;
    const float d = (s(0x40070c)-input)*0.6127790212631226f+s(0x400704);
    const float e = (s(0x400714)-d)*0.9426360130310059f+s(0x40070c);
    const float f = (s(0x40071c)-e)*0.9953020215034485f+s(0x400714);
    const float delayed=s(0x400718);
    s(0x4006f4)=d; s(0x4006f8)=e; s(0x4006fc)=f;
    s(0x40071c)=delayed; s(0x400718)=f;
    s(0x400714)=s(0x400710); s(0x400710)=e;
    s(0x40070c)=s(0x400708); s(0x400708)=d;
    s(0x400704)=s(0x400700); s(0x400700)=input;
    return parabolic(phase)*c+parabolic(phase+0.25f)*delayed;
}

void Effects::process(float* left, float* right, int count) noexcept {
    const float mix = parameters_[29], p1 = parameters_[30], p2 = parameters_[31];
    if (type_ == 0) clearDelayOnTypeChange_ = true;
    if (parameters_[36] >= 0.5f || type_ == 0 || type_ > 10) {
        std::copy_n(left, count, right);
        return;
    }
    switch (type_) {
    case 1: {
        reverb_.configure(0, -0.5f * mix + 1.0f);
        reverb_.configure(1, 0.5f * mix);
        reverb_.configure(11, p1);
        reverb_.configure(12, p2);
        std::copy_n(left, count, right);
        const float* input[] = {left, right};
        float* output[] = {left, right};
        reverb_.process(input, output, count);
        break;
    }
    case 2: { // Tempo-synchronized stereo feedback delay, 0x12396.
        const float lowpass = (1.0f / sampleRate_) * 8820.0f;
        float length = (60.0f * sampleRate_) * (1.0f / tempo_);
        length *= divisions[static_cast<int>(3.990000009536743f * p1)];
        const int period = std::clamp(static_cast<int>(length), 1, 0x40000);
        const int shortPeriod = std::clamp(static_cast<int>(0.001451247138902545f * sampleRate_),1,0x40000);
        const int halfPeriod = period / 2 + 1;
        const float dry = 1.0f-mix, complement = 1.0f-lowpass;
        for (int i=0;i<count;++i) {
            const float input = left[i] + bias;
            auto shortAt = position_[2] = (position_[2]+1u) % shortPeriod;
            const float shortSignal = tap(3,shortAt);
            tap(3,shortAt) = input;
            auto halfAt = position_[1] = (position_[1]+1u) % halfPeriod;
            float half = lowpass*tap(2,halfAt) + complement*s(0x4005e0);
            s(0x4005e0) = half;
            tap(2,halfAt) = input;
            auto at = position_[0] = (position_[0]+1u) % period;
            float l = lowpass*tap(0,at) + complement*s(0x4005e8);
            l = l*p2 + input;
            tap(0,at) = s(0x4005e8) = l;
            float r = lowpass*tap(1,at) + complement*s(0x4005ec);
            r = r*p2 + half;
            tap(1,at) = s(0x4005ec) = r;
            left[i] = (l-input)*mix + input*dry;
            right[i] = r*mix + shortSignal*dry;
        }
        break;
    }
    case 3: { // Pattern gate, 0x12239. Mix controls transition speed.
        float step = 0.25f*tempo_;
        float divisor = sampleRate_*divisions[static_cast<int>(p2*3.990000009536743f)];
        divisor *= 30.0f;
        step /= divisor;
        float smooth = ((0.009999999776482582f+mix)*176.40000915527344f)/sampleRate_;
        for (int i=0;i<count;++i) {
            s(0x4005f0) = fraction(step+s(0x4005f0));
            int pattern = static_cast<int>(p1*3.990000009536743f);
            int cell = static_cast<int>(s(0x4005f0)*8.0f);
            s(0x40060c) = (patterns[pattern*8+cell]-s(0x40060c))*smooth+s(0x40060c);
            left[i] = right[i] = (s(0x40060c)+bias)*left[i];
        }
        break;
    }
    case 4: // Stereo chorus, 0x11fdd.
    case 6: { // Feedback flanger, 0x11b92.
        const bool feedback = type_ == 6;
        float base = static_cast<float>(static_cast<int>((feedback ? 0.0006802721181884408f : 0.0022675737272948027f)*sampleRate_));
        const float depth = (sampleRate_*p2)*0.00290249427780509f;
        const float step = (p1*constructorReciprocalRate)*(feedback ? 1.7639999389648438f : 4.409999847412109f);
        const float dry = 1.0f-mix;
        for (int i=0;i<count;++i) {
            const float input = left[i]+bias;
            const auto at = position_[0]&511u;
            tap(0,at) = feedback ? s(0x4005e0)*0.800000011920929f+input : input;
            if (feedback) tap(1,at) = s(0x4005e4)*0.800000011920929f+input;
            const float lfo = triangle(s(0x4005dc));
            s(0x4005dc) = fraction(step+s(0x4005dc));
            float l = interpolated(0, (lfo+1.0f)*depth+base);
            float r = interpolated(feedback ? 1u : 0u, (1.0f-lfo)*depth+base);
            const float original = dry*input;
            if (feedback) {
                left[i] = (mix*l+original)*1.600000023841858f;
                right[i] = (original+mix*r)*1.600000023841858f;
                l = l*0.30000001192092896f + s(0x4005e8)*0.699999988079071f;
                r = r*0.30000001192092896f + s(0x4005ec)*0.699999988079071f;
                s(0x4005e0)=s(0x4005e8)=l;
                s(0x4005e4)=s(0x4005ec)=r;
            } else {
                left[i] = (r*mix+original)*1.600000023841858f;
                right[i] = (original+l*mix)*1.600000023841858f;
            }
            ++position_[0];
        }
        break;
    }
    case 5: { // Quadrature frequency shifter, 0x11e8a.
        const float step=(p1*constructorReciprocalRate)*8.819999694824219f;
        const float wet=mix+mix;
        float dry=1.0f-mix; dry+=dry;
        for(int i=0;i<count;++i) {
            const float input=bias+left[i];
            s(0x400720)=fraction(step+s(0x400720));
            float fed=(s(0x400724)*p2)*0.800000011920929f+input;
            s(0x400724)=quadrature(fed,s(0x400720));
            const float a=dry*input, b=wet*s(0x400724);
            left[i]=a-b; right[i]=a+b;
        }
        break;
    }
    case 7: { // Filtered distortion, 0x11998.
        const float gain=p1*40.0f+0.10000000149011612f;
        const float scale=((1.0f/gain)+p1*0.10000000149011612f)*1.700000047683716f;
        float cutoff=((p2*p2)*2000.0f+300.0f)*constructorReciprocalRate;
        cutoff*=0.5f;
        float coefficient=sineApprox(cutoff); coefficient+=coefficient;
        const float dry=1.0f-mix;
        for(int i=0;i<count;++i) {
            const float input=left[i]+bias;
            s(0x4005f4)=coefficient*s(0x4005f8)+s(0x4005f4);
            float high=gain*input-s(0x4005f4);
            high-=s(0x4005f8);
            s(0x4005fc)=high;
            float band=high*coefficient+s(0x4005f8);
            s(0x4005f8)=band;
            float shaped;
            if(band>0.0f) shaped=band>1.0f ? 1.0f : (band*band)*0.33329999446868896f+band*0.6665999889373779f;
            else shaped=band < -1.0f ? -1.0f : (band*band)*-0.33329999446868896f+band*0.6665999889373779f;
            left[i]=right[i]=(shaped*scale)*mix+input*dry;
        }
        break;
    }
    case 8: { // Bit/sample-rate reduction, 0x11838.
        const float levels=(p1*p1)*256.0f+8.0f, reciprocal=1.0f/levels;
        const float frequency=std::min((p2*p2)*43600.0f+500.0f,sampleRate_);
        const int period=static_cast<int>(sampleRate_/frequency);
        const float average=1.0f/static_cast<float>(period), dry=1.0f-mix;
        for(int i=0;i<count;++i) {
            const float input=left[i]+bias;
            float quantized=static_cast<float>(static_cast<int>(levels*input))*reciprocal+bias;
            ++crusherCount_;
            quantized+=s(0x400604);
            s(0x400604)=quantized;
            if(crusherCount_>=static_cast<unsigned>(period)) {
                s(0x400600)=quantized*average;
                s(0x400604)=0; crusherCount_=0;
            }
            const float original=input*dry;
            left[i]=mix*s(0x400600)+original;
            right[i]=original+mix*s(0x400600);
        }
        break;
    }
    case 9: { // Modulated stereo resonant filter/delay, 0x1147c.
        const float depth=((1.0f-p1)*sampleRate_)*0.0009070294909179211f;
        const float step=((p1+0.20000000298023224f)*constructorReciprocalRate)*8.819999694824219f;
        const float rate=constructorReciprocalRate*44100.0f, dry=1.0f-mix;
        for(int i=0;i<count;++i) {
            const float input=left[i]+bias;
            tap(0,position_[0]&511u)=input;
            const float delayLfo=triangle(s(0x4005dc)+0.25f);
            s(0x4005dc)=fraction(step+s(0x4005dc));
            float a=interpolated(0,(delayLfo+1.0f)*depth,3)+0.0010000000474974513f;
            float b=interpolated(0,(1.0f-delayLfo)*depth,3)+0.0010000000474974513f;
            ++position_[0];
            float lfo=triangle(s(0x4005dc));
            float c=(lfo+1.0f)*0.5f;
            c=(((c*c)*0.8999999761581421f+0.10000000149011612f)*rate)*1.399999976158142f;
            float d=(1.0f-lfo)*0.5f;
            d=(((d*d)*0.8999999761581421f+0.10000000149011612f)*rate)*1.399999976158142f;
            s(0x4006a4)=c*s(0x4006b0)+s(0x4006a4);
            b=b*0.699999988079071f-s(0x4006a4);
            b+=s(0x4006b0)*-0.699999988079071f;
            s(0x4006bc)=b;
            s(0x4006b0)=b*c+s(0x4006b0);
            s(0x4006a8)=d*s(0x4006b4)+s(0x4006a8);
            a=a*0.699999988079071f-s(0x4006a8);
            a+=s(0x4006b4)*-0.699999988079071f;
            s(0x4006c0)=a;
            s(0x4006b4)=a*d+s(0x4006b4);
            lfo*=p2;
            const float original=dry*input;
            left[i]=(((lfo+1.0f)*s(0x4006a4))*mix+original)*1.7999999523162842f;
            right[i]=(original+((1.0f-lfo)*s(0x4006a8))*mix)*1.7999999523162842f;
        }
        break;
    }
    case 10: { // Three moving formants, 0x11154.
        const float step=(constructorReciprocalRate*p1)*2.2049999237060547f;
        const float range=0.5f+1.5f*p2, rate=constructorReciprocalRate*6.0f, dry=1.0f-mix;
        for(int i=0;i<count;++i) {
            const float lfo=triangle(s(0x4005dc))+1.0f;
            const float half=0.5f*lfo, square=half*half;
            s(0x4005dc)=fraction(step+s(0x4005dc));
            float a=(lfo*-700.0f+2250.0f)*range;
            float b=(lfo*-350.0f+2900.0f)*range;
            float c=((square*-1468.0f+lfo*756.5f)+270.0f)*range;
            const float input=left[i]+0.0010000000474974513f;
            a*=rate; b*=rate; c*=rate;
            const float feed=input*0.050000011920928955f;
            s(0x4006a4)=a*s(0x4006b0)+s(0x4006a4);
            s(0x4006bc)=(feed-s(0x4006a4))-s(0x4006b0)*0.050000011920928955f;
            s(0x4006b0)=a*s(0x4006bc)+s(0x4006b0);
            s(0x4006a8)=b*s(0x4006b4)+s(0x4006a8);
            s(0x4006c0)=(feed-s(0x4006a8))-s(0x4006b4)*0.050000011920928955f;
            s(0x4006b4)=b*s(0x4006c0)+s(0x4006b4);
            s(0x4006ac)=c*s(0x4006b8)+s(0x4006ac);
            s(0x4006c4)=(feed-s(0x4006ac))-s(0x4006b8)*0.050000011920928955f;
            s(0x4006b8)=c*s(0x4006c4)+s(0x4006b8);
            float combined=(square*-0.30000001192092896f+1.0f)*s(0x4006b0);
            combined+=s(0x4006b4)*0.8999999761581421f;
            combined+=((square-half)*-1.7999999523162842f+0.20000000298023224f)*s(0x4006b8);
            left[i]=right[i]=(combined*8.0f)*mix+input*dry;
        }
        break;
    }
    }
}
} // namespace plasticz
