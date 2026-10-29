#pragma once
#include "pitch_processor.h"
#include <array>

namespace spt {
namespace fx {
constexpr float pi = 3.14159265358979323846f;
inline float clean(float x) { return std::abs(x) < 1e-20f ? 0.f : x; }

struct Ramp {
    float value = 0, target = 0, step = 0;
    int remaining = 0;
    void set(float next, int samples) {
        if (next == target) return;
        target = next; remaining = std::max(1,samples); step = (target-value)/remaining;
    }
    float tick() {
        if (remaining && !--remaining) value = target;
        else if (remaining) value += step;
        return value;
    }
};
struct Delay {
    std::vector<float> data;
    size_t head = 0;
    void configure(int capacity) { data.assign(size_t(capacity)+4,0); head=0; }
    void reset() { std::fill(data.begin(),data.end(),0.f); head=0; }
    float read(float samples) const {
        samples=std::clamp(samples,1.f,float(data.size()-2));
        int n=int(samples); float f=samples-n;
        size_t a=(head+data.size()-size_t(n))%data.size(), b=(a+data.size()-1)%data.size();
        return data[a]+f*(data[b]-data[a]);
    }
    void write(float x) { data[head]=clean(x); if(++head==data.size()) head=0; }
    float allpass(float x,float samples) {
        float delayed=read(samples), y=delayed-.6f*x;
        write(x+.6f*y); return y;
    }
};

// Four-times oversampled saturator: 129-tap Blackman FIR interpolation and
// decimation. Dry is aligned to the exact 32-source-frame round-trip delay.
class Saturator {
    static constexpr int taps=129, history=33;
    std::array<float,taps> fir_{};
    struct Channel {
        std::array<float,history> input{};
        std::array<float,taps> output{};
        std::array<float,32> dry{};
        int in=0,out=0,delay=0;
        float dcIn=0,dcOut=0;
    };
    std::array<Channel,8> channels_{};
public:
    void configure() {
        double sum=0;
        for(int n=0;n<taps;++n) {
            int x=n-(taps-1)/2;
            double sinc=x ? std::sin(2*.105*pi*x)/(pi*x) : .21;
            double window=.42-.5*std::cos(2*pi*n/(taps-1))+.08*std::cos(4*pi*n/(taps-1));
            fir_[n]=float(sinc*window); sum+=fir_[n];
        }
        for(auto& x:fir_) x/=float(sum);
        reset();
    }
    void reset() { channels_={}; }
    float process(int c,float x,float drive,float compensation,float mix,float dcPole) {
        auto& s=channels_[c];
        float dry=s.dry[s.delay]; s.dry[s.delay]=x; s.delay=(s.delay+1)%32;
        s.input[s.in]=x;
        float wet=0;
        for(int phase=0;phase<4;++phase) {
            float up=0;
            for(int k=phase,j=0;k<taps;k+=4,++j) up+=fir_[k]*s.input[(s.in+history-j)%history]*4;
            s.output[s.out]=std::tanh(up*drive)*compensation;
            if(phase==0) for(int k=0;k<taps;++k) wet+=fir_[k]*s.output[(s.out+taps-k)%taps];
            s.out=(s.out+1)%taps;
        }
        s.in=(s.in+1)%history;
        float blocked=wet-s.dcIn+dcPole*s.dcOut; s.dcIn=wet; s.dcOut=clean(blocked);
        return dry+mix*(blocked-dry);
    }
};

// TPT state-variable sections remain stable while cutoff changes. The two Qs
// form a fourth-order Butterworth low-pass (flat passband, 24 dB/oct roll-off).
struct LowPass {
    float s1=0,s2=0;
    float process(float x,float g,float k) {
        float v1=(s1+g*(x-s2))/(1+g*(g+k)), v2=s2+g*v1;
        s1=clean(2*v1-s1); s2=clean(2*v2-s2); return v2;
    }
};

// Stereo eight-line orthogonal feedback-delay network, with input diffusion,
// separate early reflections, damped feedback and gentle interpolated modulation.
// No stereo-to-mono sum is used: side-only material also excites the room.
class Room {
    std::array<Delay,8> tank_;
    std::array<Delay,4> diffusion_;
    std::array<Delay,2> pre_;
    std::array<float,8> damp_{},phase_{},length_{},gain_{},increment_{};
    std::array<float,2> inputLow_{};
    bool inputPrimed_=false;
    float lowPole_=0, highPole_=0, preDelay_=1, sampleRate_=48000;
    static constexpr std::array<float,8> seconds_={.0297f,.0371f,.0411f,.0437f,.0531f,.0593f,.0677f,.0739f};
public:
    void configure(float maxRate) {
        for(auto& d:tank_) d.configure(int(maxRate*.09f));
        for(auto& d:diffusion_) d.configure(int(maxRate*.018f));
        for(auto& d:pre_) d.configure(int(maxRate*.06f));
        reset();
    }
    void reset() {
        for(auto& d:tank_) d.reset();
        for(auto& d:diffusion_) d.reset();
        for(auto& d:pre_) d.reset();
        damp_={}; inputLow_={}; inputPrimed_=false;
        for(int i=0;i<8;++i) phase_[i]=i*.73f;
    }
    void tune(float rate,float amount) {
        sampleRate_=rate;
        lowPole_=std::exp(-2*pi*(6500-2500*amount)/rate);
        highPole_=std::exp(-2*pi*160/rate);
        preDelay_=.022f*rate;
        float decay=1.2f+2.2f*amount;
        for(int i=0;i<8;++i) {
            length_[i]=seconds_[i]*rate;
            gain_[i]=std::pow(10.f,-3*seconds_[i]/decay);
            increment_[i]=2*pi*(.11f+.017f*i)/rate;
        }
    }
    std::array<float,2> process(float left,float right) {
        std::array<float,2> input={left,right}, early{};
        // Prime the DC blocker on entry so enabling a room on a biased signal
        // cannot manufacture a bass transient and repeat it as early reflections.
        if(!inputPrimed_) { inputLow_=input; inputPrimed_=true; }
        for(int c=0;c<2;++c) early[c]=.22f*pre_[c].read(sampleRate_*(c ? .0137f:.0113f))+
            .15f*pre_[1-c].read(sampleRate_*(c ? .0311f:.0277f));
        for(int c=0;c<2;++c) {
            inputLow_[c]=clean(input[c]+highPole_*(inputLow_[c]-input[c]));
            float high=input[c]-inputLow_[c];
            float delayed=pre_[c].read(preDelay_); pre_[c].write(high);
            input[c]=diffusion_[c*2].allpass(delayed,sampleRate_*(c ? .0073f:.0059f));
            input[c]=diffusion_[c*2+1].allpass(input[c],sampleRate_*(c ? .0117f:.0097f));
        }
        std::array<float,8> feedback{};
        for(int i=0;i<8;++i) {
            float modulation=std::sin(phase_[i])*sampleRate_*.00022f;
            phase_[i]+=increment_[i]; if(phase_[i]>2*pi) phase_[i]-=2*pi;
            float x=tank_[i].read(length_[i]+modulation);
            damp_[i]=clean(x+lowPole_*(damp_[i]-x)); feedback[i]=damp_[i];
        }
        float l=(feedback[0]+feedback[1]-feedback[2]-feedback[3]+feedback[4]-feedback[5]+feedback[6]-feedback[7])*.35355339f;
        float r=(feedback[0]-feedback[1]+feedback[2]-feedback[3]-feedback[4]+feedback[5]+feedback[6]-feedback[7])*.35355339f;
        // Normalized Hadamard matrix: feedback energy cannot increase at the mixer.
        for(int span=1;span<8;span*=2) for(int base=0;base<8;base+=2*span)
            for(int j=0;j<span;++j) { float a=feedback[base+j],b=feedback[base+j+span]; feedback[base+j]=a+b; feedback[base+j+span]=a-b; }
        for(int i=0;i<8;++i) {
            float injection=input[i&1]*((i&2) ? -.35355339f:.35355339f);
            tank_[i].write(injection+feedback[i]*.35355339f*gain_[i]);
        }
        return {l+early[0],r+early[1]};
    }
};
}

class AudioEffects {
    int channels_=0,bits_=0,controlCounter_=0;
    float baseRate_=48000,rate_=48000;
    fx::Ramp reverb_,distortion_,muffle_,rateRamp_;
    fx::Saturator saturator_;
    std::array<fx::Room,4> rooms_;
    std::array<std::array<fx::LowPass,2>,8> lowpass_{};
    bool distortionRunning_=false,reverbRunning_=false,muffleRunning_=false;
    int saturationWarmup_=0;
    float distortionBlend_=0;
    float drive_=1,compensation_=1,dcPole_=0,cutoffG_=1;
    uint32_t requested_=0;
public:
    void configure(int channels,int bits,int rate) {
        channels_=channels; bits_=bits; baseRate_=rate_=float(rate);
        saturator_.configure();
        for(int i=0;i<(channels+1)/2;++i) rooms_[i].configure(rate*1.5f);
        reset();
    }
    void reset() {
        saturator_.reset(); for(int i=0;i<(channels_+1)/2;++i) rooms_[i].reset(); lowpass_={};
        reverb_={}; distortion_={}; muffle_={}; rateRamp_={}; rateRamp_.value=rateRamp_.target=rate_;
        distortionRunning_=reverbRunning_=muffleRunning_=false; saturationWarmup_=controlCounter_=0; requested_=0;
        distortionBlend_=0;
    }
    bool wet() const { return reverb_.value!=0 || distortion_.value!=0 || muffle_.value!=0; }
    bool settled() const { return !reverb_.remaining && !distortion_.remaining && !muffle_.remaining && !rateRamp_.remaining && !saturationWarmup_; }
    int latency() const { return distortionRunning_ ? 32 : 0; }
    uint32_t accepted() const { return requested_; } // Meaningful only with settled + packet acknowledgement.
    float muffleCutoff() const { return std::min(20000.f,rate_*.44f)*std::pow(350.f/std::min(20000.f,rate_*.44f),muffle_.value); }
    // Returns whether processing ran. With all amounts zero and ramps finished,
    // the packet is left byte-for-byte untouched, including floating-point values.
    bool process(uint8_t* bytes,unsigned frames,uint32_t amounts,float effectiveRate,const PitchProcessor& codec) {
        effectiveRate=std::clamp(effectiveRate,baseRate_*.5f,baseRate_*1.5f);
        int ramp=std::max(1,int(effectiveRate*.04f));
        reverb_.set(float(amounts&255)/100,ramp); distortion_.set(float((amounts>>8)&255)/100,ramp); muffle_.set(float((amounts>>16)&255)/100,ramp);
        rateRamp_.set(effectiveRate,std::max(1,int(effectiveRate*.12f))); requested_=amounts;
        if (!amounts && !wet()) {
            rate_=effectiveRate; rateRamp_.value=rateRamp_.target=effectiveRate; rateRamp_.remaining=0;
            return false;
        }
        if (distortion_.target && !distortionRunning_) { saturator_.reset(); distortionRunning_=true; saturationWarmup_=32; controlCounter_=0; }
        if (reverb_.target && !reverbRunning_) { for(int i=0;i<(channels_+1)/2;++i) rooms_[i].reset(); reverbRunning_=true; controlCounter_=0; }
        if (muffle_.target && !muffleRunning_) { lowpass_={}; muffleRunning_=true; controlCounter_=0; }
        for(unsigned n=0;n<frames;++n) {
            float rev=reverb_.tick(),dist=distortion_.tick(),muf=muffle_.tick(); rate_=rateRamp_.tick();
            if(!controlCounter_--) {
                controlCounter_=15;
                drive_=std::pow(10.f,dist*24.f/20); compensation_=1/std::sqrt(drive_);
                dcPole_=std::exp(-2*fx::pi*18/rate_);
                cutoffG_=std::tan(fx::pi*muffleCutoff()/rate_);
                if(reverbRunning_) for(int i=0;i<(channels_+1)/2;++i) rooms_[i].tune(rate_,rev);
            }
            std::array<float,8> audio{};
            // A separate 15 ms delay crossfade avoids an onset jump after FIR
            // warm-up, and leaves even a 1% amount fully delay-aligned at rest.
            float targetBlend=distortion_.target && !saturationWarmup_ ? 1.f : 0.f;
            float blendStep=1/(rate_*.015f);
            distortionBlend_+=std::clamp(targetBlend-distortionBlend_,-blendStep,blendStep);
            auto* sample=bytes+size_t(n)*channels_*(bits_/8);
            for(int c=0;c<channels_;++c) {
                float x=codec.decode(sample+c*(bits_/8));
                if(distortionRunning_) {
                    float shaped=saturator_.process(c,x,drive_,compensation_,.85f*std::sqrt(dist),dcPole_);
                    x+=distortionBlend_*(shaped-x);
                }
                audio[c]=x;
            }
            if(saturationWarmup_) --saturationWarmup_;
            if(reverbRunning_) for(int c=0;c<channels_;c+=2) {
                auto room=rooms_[c/2].process(audio[c],audio[std::min(c+1,channels_-1)]);
                float mix=.55f*rev;
                audio[c]=audio[c]*(1-mix)+room[0]*mix;
                if(c+1<channels_) audio[c+1]=audio[c+1]*(1-mix)+room[1]*mix;
            }
            for(int c=0;c<channels_;++c) {
                float x=audio[c];
                if(muffleRunning_) {
                    float filtered=lowpass_[c][0].process(x,cutoffG_,1.f/.5411961f);
                    filtered=lowpass_[c][1].process(filtered,cutoffG_,1.f/1.306563f);
                    x+=std::min(1.f,muf/.035f)*(filtered-x);
                }
                // Soft peak protection fades in with the amounts; no limiter at bypass.
                float magnitude=std::abs(x);
                if(magnitude>.94f) {
                    float protectedSample=std::copysign(.94f+.06f*std::tanh((magnitude-.94f)/.06f),x);
                    x+=std::min(1.f,std::max({rev,dist,muf})/.05f)*(protectedSample-x);
                }
                if(rev || dist || muf) codec.encode(sample+c*(bits_/8),x);
            }
        }
        if(!distortion_.value && !distortion_.target) distortionRunning_=false;
        if(!reverb_.value && !reverb_.target) reverbRunning_=false;
        if(!muffle_.value && !muffle_.target) muffleRunning_=false;
        return true;
    }
};
}
