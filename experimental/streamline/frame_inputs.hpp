#pragma once
#include <array>
#include <mutex>
#include "camera_hook.hpp"

namespace fsrSlCamera {
// Bounded per-frame handoff. The consumer supplies identity from its own standard
// frame/resource API, never from an upscaler-call counter or the latest sample.
class FrameInputs {
 struct Entry {Sample sample{};bool occupied=false,consumed=false;};
 std::array<Entry,64> entries{};
 size_t next=0;
 std::mutex mutex;
public:
 void publish(const Sample& sample){
  std::lock_guard<std::mutex> lock(mutex);
  for(auto& e:entries)if(e.occupied&&e.sample.frame==sample.frame&&e.sample.viewport==sample.viewport){
   if(!e.consumed)e.sample=sample;
   return;
  }
  entries[next]={sample,true,false};next=(next+1)%entries.size();
 }
 bool merge(uint32_t frame,uint32_t viewport,ffxDispatchDescFrameGenerationPrepareV2& input){
  // Clear inherited values even when no exact match is available.
  for(unsigned i=0;i<3;++i)input.cameraPosition[i]=input.cameraUp[i]=input.cameraRight[i]=input.cameraForward[i]=0;
  if(input.frameID!=frame)return false;
  std::lock_guard<std::mutex> lock(mutex);
  for(auto& e:entries)if(e.occupied&&e.sample.frame==frame&&e.sample.viewport==viewport){
   if(e.consumed)return false;e.consumed=true;
   if(!e.sample.valid||!fsrFgCamera::valid(e.sample.camera))return false;
   for(unsigned i=0;i<3;++i){input.cameraPosition[i]=e.sample.camera.cameraPosition[i];input.cameraUp[i]=e.sample.camera.cameraUp[i];input.cameraRight[i]=e.sample.camera.cameraRight[i];input.cameraForward[i]=e.sample.camera.cameraForward[i];}
   input.reset=input.reset||e.sample.camera.reset;
   return true;
  }
  return false;
 }
 void clear(){std::lock_guard<std::mutex> lock(mutex);entries={};next=0;}
};
}
