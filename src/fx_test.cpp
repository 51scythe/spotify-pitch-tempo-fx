#define NOMINMAX
#include "audio_effects.h"
#include <cstdio>
#include <complex>
#include <chrono>

namespace {
constexpr double pi=3.14159265358979323846;
int failures=0;
void check(bool pass,const char* name) { std::printf("%s %s\n",pass?"PASS":"FAIL",name); if(!pass) ++failures; }
double rms(const std::vector<float>& x,size_t begin,size_t end,int channels=2,int channel=0) {
    double sum=0; for(size_t n=begin;n<end;++n) sum+=double(x[n*channels+channel])*x[n*channels+channel];
    return std::sqrt(sum/(end-begin));
}
double amplitude(const std::vector<float>& x,int rate,double hz,int start,int count) {
    std::complex<double> sum{0,0},phase{1,0},step=std::polar(1.0,-2*pi*hz/rate);
    for(int i=0;i<count;++i) { sum+=double(x[(start+i)*2])*phase; phase*=step; }
    return 2*std::abs(sum)/count;
}
double db(double value) { return 20*std::log10(std::max(value,1e-30)); }
std::vector<float> tone(int frames,int rate,double hz,double level=.3) {
    std::vector<float> x(size_t(frames)*2);
    for(int n=0;n<frames;++n) x[n*2]=x[n*2+1]=float(level*std::sin(2*pi*hz*n/rate));
    return x;
}
struct Rig {
    spt::PitchProcessor codec;
    spt::AudioEffects fx;
    int rate;
    explicit Rig(int sampleRate=48000):rate(sampleRate) { codec.configure(2,rate,32,true); fx.configure(2,32,rate); }
    void run(std::vector<float>& x,uint32_t amounts,float effective=0) {
        int frames=int(x.size()/2);
        for(int n=0;n<frames;n+=480) fx.process(reinterpret_cast<uint8_t*>(x.data()+n*2),unsigned(std::min(480,frames-n)),amounts,effective?effective:float(rate),codec);
    }
    void warm(uint32_t amounts,float effective=0) { std::vector<float> silence(size_t(rate)*2); run(silence,amounts,effective); }
};
}
int main() {
    // Byte identity includes float NaNs, over-unity data, signed zeros and PCM padding.
    for(int bits:{16,24,32}) for(bool floating:{false,true}) {
        if(floating && bits!=32) continue;
        spt::PitchProcessor codec;codec.configure(2,48000,bits,floating,bits==32&&!floating?24:bits);
        spt::AudioEffects fx;fx.configure(2,bits,48000);
        std::vector<uint8_t> original(size_t(480*2*bits/8)); uint32_t seed=51;
        for(auto& x:original) { seed=1664525*seed+1013904223; x=uint8_t(seed>>24); }
        auto audio=original;bool pass=!fx.process(audio.data(),480,0,48000,codec) && audio==original;
        std::vector<uint8_t> zero(size_t(48000*2*bits/8));
        fx.process(zero.data(),48000,0x646464,48000,codec);
        fx.process(zero.data(),48000,0,72000,codec);
        audio=original;fx.process(audio.data(),480,0,72000,codec);
        pass &= audio==original && !fx.wet() && fx.settled() && fx.accepted()==0 && fx.latency()==0;
        check(pass,floating?"float exact bypass before/after effects":"PCM exact bypass before/after effects");
    }
    {
        Rig rig;auto audio=tone(48000,48000,1000);
        rig.fx.process(reinterpret_cast<uint8_t*>(audio.data()),480,0x646464,48000,rig.codec);
        bool pass=!rig.fx.settled();rig.run(audio,0x646464);
        pass &= rig.fx.settled() && rig.fx.accepted()==0x646464 && rig.fx.latency()==32;
        rig.fx.reset();std::fill(audio.begin(),audio.end(),0.f);rig.run(audio,0x646464);
        for(float x:audio) pass &= x==0;
        check(pass,"settling acknowledgement and reset clears all effect history");
    }
    // Independently measure the response rather than inspecting coefficients.
    for(int effective:{24000,48000,72000}) for(int amount:{50,100}) {
        double cutoff=std::min(20000.,effective*.44)*std::pow(350/std::min(20000.,effective*.44),amount/100.);
        double gainAtCut=0,gainLow=0,gainHigh=0;
        for(int kind=0;kind<3;++kind) {
            double hz=kind==0?cutoff:kind==1?cutoff/4:std::min(cutoff*4,effective*.4);
            Rig rig;uint32_t amounts=uint32_t(amount)<<16;rig.warm(amounts,float(effective));
            auto audio=tone(effective, effective,hz);auto original=audio;rig.run(audio,amounts,float(effective));
            double gain=db(rms(audio,effective/2,effective)/rms(original,effective/2,effective));
            if(kind==0)gainAtCut=gain;else if(kind==1)gainLow=gain;else gainHigh=gain;
        }
        std::printf("muffle amount=%d effectiveRate=%d cutoff=%.1fHz gains low=%.2f cutoff=%.2f high=%.2fdB\n",amount,effective,cutoff,gainLow,gainAtCut,gainHigh);
        check(std::abs(gainAtCut+3.0103)<.12 && std::abs(gainLow)<.12 && gainHigh<-40,"muffle measured response / tempo compensation");
    }
    {
        Rig rig;rig.warm(100<<8);auto audio=tone(96000,48000,9000,.7);auto naive=audio;
        float drive=std::pow(10.f,24.f/20),comp=1/std::sqrt(drive);
        for(auto& x:naive) x=.15f*x+.85f*std::tanh(x*drive)*comp;
        rig.run(audio,100<<8);
        double alias=0,baseAlias=0;
        for(double hz:{3000.,15000.,21000.}) { alias+=std::pow(amplitude(audio,48000,hz,48000,48000),2);baseAlias+=std::pow(amplitude(naive,48000,hz,48000,48000),2); }
        double reduction=db(std::sqrt(alias/baseAlias));
        std::printf("distortion 9kHz alias energy reduction versus same non-oversampled tanh: %.2fdB\n",reduction);
        check(reduction<-25,"distortion oversampling suppresses folded harmonics");
        double previous=-1;
        for(int amount:{10,50,100}) {
            Rig test;test.warm(uint32_t(amount)<<8);auto x=tone(96000,48000,1000,.4);test.run(x,uint32_t(amount)<<8);
            double harmonic=amplitude(x,48000,3000,48000,48000)/amplitude(x,48000,1000,48000,48000);
            std::printf("distortion amount=%d third/fundamental=%.2fdB RMS=%.4f\n",amount,db(harmonic),rms(x,48000,96000));
            check(harmonic>previous && rms(x,48000,96000)>.1 && rms(x,48000,96000)<.6,"distortion amount increases coloration with controlled level");previous=harmonic;
        }
    }
    {
        std::vector<double> decay;
        for(int amount:{25,100}) {
            Rig rig;rig.warm(uint32_t(amount));std::vector<float> x(48000*8*2);x[0]=.7f;x[1]=-.7f;rig.run(x,uint32_t(amount));
            double early=rms(x,4800,24000),late=rms(x,48000*6,48000*8),tail=rms(x,48000,48000*2);
            double lr=0,ll=0,rr=0;for(int n=4800;n<48000;++n) { lr+=x[n*2]*x[n*2+1];ll+=x[n*2]*x[n*2];rr+=x[n*2+1]*x[n*2+1]; }
            double correlation=lr/std::sqrt(ll*rr),ratio=db(tail/early);decay.push_back(ratio);
            std::printf("reverb amount=%d side-only excitation early=%.7f decay1s=%.2fdB end=%.2fdB stereoCorr=%.3f\n",amount,early,ratio,db(late/early),correlation);
            check(early>1e-5 && db(late/early)<-60 && std::abs(correlation)<.95,"reverb side energy, stereo diffusion and stable decay");
        }
        check(decay[1]>decay[0]+5,"higher reverb amount has a longer measured tail");
    }
    // Abrupt slider changes and rate sweeps, including every effect reaching zero.
    for(uint32_t amounts:{100u,100u<<8,100u<<16,0x646464u}) {
        Rig rig;std::vector<float> x(48000*4*2,.98f);double largest=0;float previous=.98f;
        for(int n=0;n<48000*4;n+=120) {
            uint32_t a=(n/4800)%2 ? amounts:0;
            rig.fx.process(reinterpret_cast<uint8_t*>(x.data()+n*2),120,a,48000,rig.codec);
            for(int j=n;j<n+120;++j) { largest=std::max(largest,double(std::abs(x[j*2]-previous)));previous=x[j*2]; }
        }
        std::printf("transition amounts=0x%06x constant-signal maximum sample step=%.6f\n",amounts,largest);
        check(largest<.03,"smoothed on/off transitions without an onset step");
    }
    for(int rate:{8000,44100,48000,96000,192000}) {
        Rig rig(rate);bool pass=true;uint32_t seed=91;
        for(int block=0;block<160;++block) {
            std::vector<float> x(257*2);for(auto& v:x) { seed=1664525*seed+1013904223;v=(float(seed>>8)/8388608-1)*.9f; }
            float effective=rate*(block%3==0?.5f:block%3==1?1.f:1.5f);
            rig.run(x,block%7==0?0:0x646464,effective);
            for(float v:x)pass &= std::isfinite(v) && std::abs(v)<=1;
        }
        std::printf("stress baseRate=%d\n",rate);check(pass,"combined effects / rapid rate and amount changes remain finite and bounded");
    }
    for(int channels:{1,2,8}) for(int bits:{16,24,32}) {
        spt::PitchProcessor codec;codec.configure(channels,48000,bits,false,bits==32?24:bits);
        spt::AudioEffects fx;fx.configure(channels,bits,48000);
        std::vector<uint8_t> x(size_t(48000)*channels*(bits/8));
        for(int n=0;n<48000;++n) for(int c=0;c<channels;++c) codec.encode(x.data()+size_t(n*channels+c)*(bits/8),float(.6*std::sin(2*pi*(440+220*c)*n/48000)));
        fx.process(x.data(),48000,0x646464,48000,codec);bool pass=fx.settled();double energy=0;
        for(size_t n=0;n<x.size();n+=bits/8) { float value=codec.decode(x.data()+n);pass &= std::isfinite(value) && std::abs(value)<=1;energy+=value*value;if(bits==32)pass &= x[n]==0; }
        std::printf("wet PCM channels=%d bits=%d energy=%.3f\n",channels,bits,energy);
        check(pass && energy>1,"effect channel bounds / PCM quantization and padding");
    }
    {
        Rig rig;rig.warm(0x646464);std::vector<double> times;auto source=tone(480,48000,440);
        for(int i=0;i<600;++i) {
            auto x=source;auto begin=std::chrono::steady_clock::now();
            rig.codec.process(reinterpret_cast<uint8_t*>(x.data()),480,1.3);
            rig.fx.process(reinterpret_cast<uint8_t*>(x.data()),480,0x646464,72000,rig.codec);
            times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
        }
        std::sort(times.begin(),times.end());double average=0;for(double t:times)average+=t/times.size();
        std::printf("48k stereo pitch+all-FX 480-frame packet at 1.5x: mean=%.3fms p99=%.3fms max=%.3fms budget=6.667ms\n",average,times[594],times.back());
        check(times[594]<6.667,"offline realtime-cost budget at 1.5x");
    }
    std::printf("%d failure(s); offline synthetic DSP tests, not live Spotify listening proof\n",failures);
    return failures?1:0;
}
