#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <stdexcept>
static void need(bool b,const char* m){if(!b)throw std::runtime_error(m);}
static double finish(id<MTLCommandBuffer> c){auto s=dispatch_semaphore_create(0);[c addCompletedHandler:^(id<MTLCommandBuffer>){dispatch_semaphore_signal(s);}];[c commit];need(dispatch_semaphore_wait(s,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC))==0,"GPU timeout");need(c.status==MTLCommandBufferStatusCompleted,"GPU command failed");return(c.GPUEndTime-c.GPUStartTime)*1000;}
int main(int argc,const char** argv){@autoreleasepool{
 if(argc!=6)return 2;NSMutableDictionary* result=[@{@"passed":@NO} mutableCopy];int rc=1;
 try{
 NSString* package=@(argv[1]);NSString* inputDir=@(argv[2]);NSString* sources=@(argv[3]);bool full=std::strcmp(argv[4],"full")==0;
 unsigned width=full?1720:64,height=full?720:48,count=width*height;size_t bytes=size_t(count)*16;
 auto d=MTLCreateSystemDefaultDevice();need(d&&[d.name isEqual:@"Apple M4 Pro"]&&MPSSupportsMTLDevice(d),"M4 Pro/MPS required");auto queue=[d newCommandQueue];need(queue!=nil,"queue");result[@"device"]=d.name;result[@"thermal_start"]=@(NSProcessInfo.processInfo.thermalState);
 size_t allocated=0;std::vector<std::pair<id<MTLBuffer>,size_t>> guards;
 auto buffer=[&](size_t n){allocated+=n+256;need(allocated<1536ull*1024*1024,"allocation budget");auto x=[d newBufferWithLength:n+256 options:MTLResourceStorageModeShared];need(x!=nil,"buffer");memset(x.contents,0xa5,x.length);guards.emplace_back(x,n);return x;};
 auto load=[&](NSString* path,size_t n){auto data=[NSData dataWithContentsOfFile:path];need(data&&data.length==n,"file size");auto b=buffer(n);memcpy(b.contents,data.bytes,n);return b;};
 auto path=[&](NSString* name){return[package stringByAppendingPathComponent:[@"4.0.2/weights/1/" stringByAppendingString:name]];};
 auto weights=load(path(@"weights.bin"),3328),bias=load(path(@"biases.bin"),256),scales=load(path(@"scales.bin"),36),packed=load(path(@"packed.bin"),6656);
 auto input=load([inputDir stringByAppendingPathComponent:[NSString stringWithFormat:@"%ux%u-input.bin",width,height]],bytes);
 auto expected=[NSData dataWithContentsOfFile:[inputDir stringByAppendingPathComponent:[NSString stringWithFormat:@"%ux%u-output.bin",width,height]]];need(expected&&expected.length==bytes,"reference size");
 std::vector<unsigned char> original(bytes);memcpy(original.data(),input.contents,bytes);
 auto baselineOut=buffer(bytes),mpsOut=buffer(bytes),dummy=buffer(256);
 auto compile=[&](NSString* file,NSString* name){NSError* error=nil;auto text=[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:&error];need(text&&error==nil,"shader source");auto opt=[MTLCompileOptions new];opt.languageVersion=MTLLanguageVersion2_3;opt.mathMode=MTLMathModeSafe;opt.mathFloatingPointFunctions=MTLMathFloatingPointFunctionsPrecise;auto lib=[d newLibraryWithSource:text options:opt error:&error];need(lib&&error==nil,error?error.localizedDescription.UTF8String:"shader compile");auto p=[d newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&error];need(p&&error==nil,"pipeline");return p;};
 auto baseline=compile([package stringByAppendingPathComponent:@"4.0.2/shaders/matrix_block16_register_final.metal"],@"block16_tile8x2");
 auto gather=compile([sources stringByAppendingPathComponent:@"bridge.metal"],@"gather"),quant=compile([sources stringByAppendingPathComponent:@"bridge.metal"],@"quantize");
 auto columns=buffer(size_t(count)*144*4),mixed=buffer(size_t(count)*16*4),expanded=buffer(size_t(count)*32*4);
 id<MTLBuffer> raw[3]={buffer(size_t(count)*16*4),buffer(size_t(count)*32*4),buffer(size_t(count)*16*4)};
 unsigned ks[]={144,16,32},ns[]={16,32,16},offs[]={0,2304,2816};id<MTLBuffer> w[3];MPSMatrix* lhs[3];MPSMatrix* rhs[3];MPSMatrix* dst[3];MPSMatrixMultiplication* mul[3];
 auto matrix=[&](id<MTLBuffer> b,unsigned rows,unsigned cols){auto md=[MPSMatrixDescriptor matrixDescriptorWithRows:rows columns:cols rowBytes:cols*4 dataType:MPSDataTypeFloat32];return [[MPSMatrix alloc] initWithBuffer:b descriptor:md];};
 const auto pw=static_cast<const _Float16*>(packed.contents);
 for(unsigned j=0;j<3;++j){w[j]=buffer(ks[j]*ns[j]*4);auto wp=static_cast<float*>(w[j].contents);for(unsigned i=0;i<ks[j]*ns[j];++i){wp[i]=float(pw[offs[j]+i]);need(wp[i]==std::rint(wp[i])&&wp[i]>=-128&&wp[i]<=127,"integer weight domain");}lhs[j]=matrix(j==0?columns:j==1?mixed:expanded,count,ks[j]);rhs[j]=matrix(w[j],ks[j],ns[j]);dst[j]=matrix(raw[j],count,ns[j]);mul[j]=[[MPSMatrixMultiplication alloc] initWithDevice:d transposeLeft:NO transposeRight:NO resultRows:count resultColumns:ns[j] interiorColumns:ks[j] alpha:1 beta:0];need(mul[j]!=nil,"MPS multiplication");}
 auto encode=[&](bool useMPS){auto cmd=[queue commandBuffer];need(cmd!=nil,"command");unsigned shape[]={width,height};
 if(!useMPS){auto e=[cmd computeCommandEncoder];[e setComputePipelineState:baseline];id<MTLBuffer> bs[]={input,weights,bias,scales,dummy,dummy};for(unsigned j=0;j<6;++j)[e setBuffer:bs[j] offset:0 atIndex:j];[e setBytes:shape length:8 atIndex:6];[e setBuffer:baselineOut offset:0 atIndex:8];[e setBuffer:packed offset:0 atIndex:9];[e dispatchThreadgroups:MTLSizeMake((width+7)/8,(height+1)/2,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];[e endEncoding];}
 else{auto e=[cmd computeCommandEncoder];[e setComputePipelineState:gather];[e setBuffer:input offset:0 atIndex:0];[e setBuffer:columns offset:0 atIndex:1];[e setBytes:shape length:8 atIndex:2];[e dispatchThreads:MTLSizeMake(size_t(count)*144,1,1) threadsPerThreadgroup:MTLSizeMake(128,1,1)];[e endEncoding];
 for(unsigned j=0;j<3;++j){[mul[j] encodeToCommandBuffer:cmd leftMatrix:lhs[j] rightMatrix:rhs[j] resultMatrix:dst[j]];e=[cmd computeCommandEncoder];[e setComputePipelineState:quant];id<MTLBuffer> bs[]={raw[j],j==0?mixed:expanded,mpsOut,input,bias,scales};for(unsigned k=0;k<6;++k)[e setBuffer:bs[k] offset:0 atIndex:k];unsigned cfg[]={count,j};[e setBytes:cfg length:8 atIndex:6];[e dispatchThreads:MTLSizeMake(size_t(count)*ns[j],1,1) threadsPerThreadgroup:MTLSizeMake(128,1,1)];[e endEncoding];}}
 return finish(cmd);};
 encode(false);need(memcmp(baselineOut.contents,expected.bytes,bytes)==0,"baseline/captured reference mismatch");encode(true);need(memcmp(mpsOut.contents,expected.bytes,bytes)==0,"MPS/captured reference mismatch");
 // Independent INT64 dot products and FP32 epilogues, all small pixels and64 spread full pixels.
 auto ip=static_cast<const int8_t*>(input.contents);auto bp=static_cast<const float*>(bias.contents);auto sp=static_cast<const float*>(scales.contents);unsigned checks=full?64:count;
 for(unsigned check=0;check<checks;++check){unsigned pos=full?unsigned(uint64_t(check)*(count-1)/(checks-1)):check;float q0[16],q1[32];
 for(unsigned stage=0;stage<3;++stage)for(unsigned c=0;c<ns[stage];++c){int64_t acc=0;
 for(unsigned k=0;k<ks[stage];++k){int v;
 if(stage==0){int x=int(pos%width)+int((k/16)%3)-1,y=int(pos/width)+int(k/48)-1;v=x>=0&&y>=0&&x<int(width)&&y<int(height)?ip[(y*width+x)*16+k%16]:0;}else v=int(stage==1?q0[k]:q1[k]);acc+=int64_t(v)*int(pw[offs[stage]+k*ns[stage]+c]);}
 need(static_cast<const float*>(raw[stage].contents)[pos*ns[stage]+c]==float(acc),"MPS raw sum/INT64 oracle mismatch");float v=float(acc);
 if(stage==0){v=v*(sp[0]*sp[1]);v=v+bp[c];v=v*sp[4];q0[c]=fminf(127,fmaxf(-128,std::rint(v)));}
 else if(stage==1){v=v*(sp[5]*sp[2]);v=v+bp[16+c];v=v*sp[6];q1[c]=fminf(127,fmaxf(0,std::rint(v)));}
 else{v=v*(sp[3]*sp[7]);v=v+bp[48+c];float res=float(ip[pos*16+c])*sp[0];v=v+res;v=v*(1.f/sp[8]);need(static_cast<const int8_t*>(mpsOut.contents)[pos*16+c]==int8_t(fminf(127,fmaxf(-128,std::rint(v)))),"final oracle mismatch");}
 }}
 NSMutableArray* pairs=[NSMutableArray new];for(unsigned i=0;i<(full?6u:0u);++i){double bm=0,mm=0;for(unsigned step=0;step<2;++step){bool alt=(i+step)%2;double t=encode(alt);need(memcmp((alt?mpsOut:baselineOut).contents,expected.bytes,bytes)==0,"timed output mismatch");if(alt)mm=t;else bm=t;}[pairs addObject:@{@"baseline_ms":@(bm),@"mps_ms":@(mm)}];}
 for(auto& x:guards){auto ptr=static_cast<const unsigned char*>(x.first.contents);for(size_t k=x.second;k<x.first.length;++k)need(ptr[k]==0xa5,"buffer guard changed");}
 need(memcmp(input.contents,original.data(),bytes)==0,"input mutation");result[@"passed"]=@YES;result[@"width"]=@(width);result[@"height"]=@(height);result[@"oracle_pixels"]=@(checks);result[@"exact_output_bytes"]=@(bytes);result[@"pairs"]=pairs;result[@"allocated_bytes_both_arms"]=@(allocated);result[@"thermal_end"]=@(NSProcessInfo.processInfo.thermalState);result[@"scope"]=@"Full FNB16 block: im2col + 3 FP32 MPS GEMMs + original quantization/residual; fixed weights packed once before timing";rc=0;
 }catch(const std::exception& e){result[@"error"]=@(e.what());fprintf(stderr,"FAIL %s\n",e.what());}
 NSError* err=nil;auto data=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:&err];if(!data||![data writeToFile:@(argv[5]) options:NSDataWritingAtomic error:&err])return 3;return rc;
}}
