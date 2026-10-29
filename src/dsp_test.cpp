#define NOMINMAX
#include "pitch_processor.h"
#include "control.h"
#include <cstdio>
#include <vector>
#include <cstring>
#include <complex>
#include <chrono>

double estimate(const std::vector<float>& samples, double sampleRate, double expected) {
    // Independent narrow-band DFT measurement, well after processor warm-up.
    const int n = 16384;
    double best=0, bestHz=0;
    for(double hz=expected-8;hz<=expected+8;hz+=0.1) {
        std::complex<double> sum{0,0}, phase{1,0};
        std::complex<double> step=std::polar(1.0,-2*3.141592653589793*hz/sampleRate);
        for(int i=0;i<n;++i) {
            double weight=0.5-0.5*std::cos(2*3.141592653589793*i/(n-1));
            sum += double(samples[(samples.size()/2-n+i)*2])*weight*phase; phase*=step;
        }
        if(std::norm(sum)>best) { best=std::norm(sum);bestHz=hz; }
    }
    return bestHz;
}
int main() {
    constexpr int rate=48000, frames=rate*4;
    std::vector<float> source(frames*2);
    for(int i=0;i<frames;++i) { source[i*2]=float(.2*std::sin(2*3.141592653589793*440*i/rate)); source[i*2+1]=float(.15*std::sin(2*3.141592653589793*660*i/rate)); }
    int failures=0;
    auto start=std::chrono::steady_clock::now();
    for(auto c: {spt::Settings{1000,0,0,1},spt::Settings{850,0,0,1},spt::Settings{850,-200,0,1},spt::Settings{1250,700,0,1},spt::Settings{1250,0,1,1}}) {
        double tempo=c.tempo/1000.0;
        double factor=spt::pitchFactor(c,tempo);
        auto audio=source; spt::PitchProcessor dsp;dsp.configure(2,rate,32,true);
        for(int pos=0;pos<frames;pos+=480) dsp.process(reinterpret_cast<uint8_t*>(audio.data()+pos*2),480,factor);
        double expected=440*(c.linked?tempo:std::pow(2.0,c.pitch/1200.0));
        double measured=estimate(audio,rate*tempo,expected);
        bool finite=true;for(float x:audio) if(!std::isfinite(x)||std::abs(x)>1)finite=false;
        bool exact=(factor!=1)||!std::memcmp(audio.data(),source.data(),source.size()*sizeof(float));
        // Pitch shifter quality budget: five cents (1/20 semitone). Unlike a
        // fixed-Hz bound this is equally strict across the musical range.
        double cents=1200*std::log2(measured/expected);
        bool pass=std::abs(cents)<5 && finite && exact;
        std::printf("%s tempo=%.3f pitch=%+.2f linked=%ld expected=%.3fHz measured=%.3fHz error=%+.2fc unity_exact=%d\n",pass?"PASS":"FAIL",tempo,c.pitch/100.0,c.linked,expected,measured,cents,exact);
        if(!pass)++failures;
    }
    // Packed PCM conversion boundaries and a unity bypass round-trip.
    for(int bits: {16,24,32}) {
        spt::PitchProcessor dsp;dsp.configure(2,rate,bits,false);
        uint8_t buffer[4]{};
        for(float value: {-1.f,-.5f,0.f,.5f,.999f}) {
            dsp.encode(buffer,value);float decoded=dsp.decode(buffer);
            if(std::abs(value-decoded)>1.f/32768)++failures;
        }
    }
    {
        spt::PitchProcessor dsp; dsp.configure(2,rate,32,false,24);
        bool pass=true;
        for (float value : {-1.f,-.89123f,-.1f,0.f,.000003f,.314159f,1.f}) {
            uint8_t buffer[4]{}; dsp.encode(buffer,value);
            if (buffer[0] || std::abs(dsp.decode(buffer)-value)>1.f/8388608) pass=false;
        }
        std::printf("%s PCM24-in-32 quantization and zero padding\n",pass?"PASS":"FAIL");
        if (!pass) ++failures;
    }
    {
        spt::PitchProcessor dsp; dsp.configure(2,rate,32,true);
        auto audio=source;
        dsp.process(reinterpret_cast<uint8_t*>(audio.data()),480,1.3);
        bool pass=!dsp.settled(); // Warm-up must not be acknowledged as applied.
        dsp.process(reinterpret_cast<uint8_t*>(audio.data()),rate,1.3);
        pass &= dsp.settled();
        dsp.process(reinterpret_cast<uint8_t*>(audio.data()),480,.8);
        pass &= !dsp.settled();
        dsp.process(reinterpret_cast<uint8_t*>(audio.data()),rate,1);
        audio=source;
        dsp.process(reinterpret_cast<uint8_t*>(audio.data()),rate,1);
        pass &= !dsp.wet() && dsp.settled() && !memcmp(audio.data(),source.data(),rate*2*sizeof(float));
        dsp.process(reinterpret_cast<uint8_t*>(audio.data()),rate,1.3);
        dsp.reset(); // Same discontinuity policy used for silent packets.
        audio.assign(rate*2,0);
        dsp.process(reinterpret_cast<uint8_t*>(audio.data()),rate,.8);
        for(float v : audio) if(std::abs(v)>1e-8) pass=false;
        std::printf("%s transition acknowledgement, eventual exact bypass, silent reset history\n",pass?"PASS":"FAIL");
        if(!pass) ++failures;
    }
    auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    std::printf("%d failure(s), %lldms; offline DSP/clock-model test only, not live Spotify proof\n",failures,static_cast<long long>(ms));
    return failures?1:0;
}
