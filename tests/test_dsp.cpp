#include "../src/wav.h"
#include "../src/dsp.h"
#include <cassert>
#include <cstdio>
static void writeWav16(const char* p, const std::vector<int16_t>& s, int rate, int ch) {
    FILE* f = fopen(p, "wb"); uint32_t dl = s.size()*2, rl = 36+dl, fl=16, sr=rate, br=rate*ch*2;
    uint16_t pcm=1, c=ch, ba=ch*2, b=16;
    fwrite("RIFF",1,4,f); fwrite(&rl,4,1,f); fwrite("WAVEfmt ",1,8,f); fwrite(&fl,4,1,f);
    fwrite(&pcm,2,1,f); fwrite(&c,2,1,f); fwrite(&sr,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&b,2,1,f);
    fwrite("data",1,4,f); fwrite(&dl,4,1,f); fwrite(s.data(),2,s.size(),f); fclose(f);
}
int main() {
    writeWav16("/tmp/t_st.wav", {16384,-16384,8192,8192}, 44100, 2);
    WavData w; assert(loadWav("/tmp/t_st.wav", w)); assert(w.rate==44100 && w.mono.size()==2);
    assert(std::fabs(w.mono[0])<1e-6 && std::fabs(w.mono[1]-0.25f)<1e-4);
    writeWav16("/tmp/t_mo.wav", {16384, 0, -32768}, 48000, 1);
    assert(loadWav("/tmp/t_mo.wav", w)); assert(w.mono.size()==3 && std::fabs(w.mono[0]-0.5f)<1e-4 && w.mono[2]==-1.0f);
    assert(!loadWav("/tmp/does_not_exist.wav", w));
    auto r = resampleLinear(std::vector<float>(1000,1.f), 48000, 24000); assert(r.size()==500);

    FirConv fc; fc.setIR({0.5f,0.25f,0.125f});
    float x[8]={1,0,0,0,0,0,0,0}; fc.process(x,4); fc.process(x+4,4);
    assert(std::fabs(x[0]-0.5f)<1e-6 && std::fabs(x[1]-0.25f)<1e-6 && std::fabs(x[2]-0.125f)<1e-6 && std::fabs(x[3])<1e-6);
    FirConv f2; f2.setIR({1.f,0.5f,0.25f}); float y[8]={0,0,0,1,0,0,0,0};
    f2.process(y,4); f2.process(y+4,4);
    assert(std::fabs(y[3]-1.f)<1e-6 && std::fabs(y[4]-0.5f)<1e-6 && std::fabs(y[5]-0.25f)<1e-6);

    auto gainAt=[&](double f, int mode){ ToneStack ts; ts.update(48000, mode==0?12:0, 0, mode==1?12:0);
        double a=0,b=0; for(int i=0;i<48000;i++){ float s=std::sin(2*M_PI*f*i/48000); float o=ts.process(s);
            if(i>24000){a+=s*s;b+=o*o;} } return 10*std::log10(b/a); };
    double g60=gainAt(60,0), g5k=gainAt(5000,0), t8k=gainAt(8000,1), t200=gainAt(200,1);
    printf("lowshelf 60Hz %.2f dB, 5k %.2f dB | highshelf 8k %.2f dB, 200Hz %.2f dB\n",g60,g5k,t8k,t200);
    assert(g60>10.5 && g60<12.5 && std::fabs(g5k)<0.5 && t8k>10.5 && std::fabs(t200)<0.5);

    NoiseGate g; g.init(48000); std::vector<float> q(48000), l(48000);
    for(int i=0;i<48000;i++){ q[i]=0.001f*std::sin(i*0.05f); l[i]=0.3f*std::sin(i*0.05f);}
    g.process(q.data(),48000,-50); assert(std::fabs(q[47999])<1e-4);
    NoiseGate g2; g2.init(48000); g2.process(l.data(),48000,-50);
    float mx=0; for(int i=47000;i<48000;i++) mx=std::max(mx,std::fabs(l[i])); assert(mx>0.25f);
    auto p=prepareIR(std::vector<float>(2000,0.3f)); assert(p.size()==512); double e=0; for(float v:p) e+=v*v; assert(std::fabs(e-1.0)<0.02);
    puts("ALL OK");
}
