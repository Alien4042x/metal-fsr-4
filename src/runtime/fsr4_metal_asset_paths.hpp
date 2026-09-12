#pragma once
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdexcept>
#include <cstdlib>
#include <mach-o/getsect.h>
#include <CommonCrypto/CommonDigest.h>
// WineForge-Internal: fsr4/module-relative-runtime-assets-v1.
static NSString* fsr4AssetPath(NSString* category, NSString* name) {
 static NSString* root = []() -> NSString* {
  // Explicit asset location allows manually placed PE/native libraries.
  // No dependency on the WineForge application or its bundle path.
  if(const char* configured=std::getenv("METAL_FSR4_ASSET_ROOT")){
   NSString* path=[[NSString stringWithUTF8String:configured] stringByStandardizingPath];
   BOOL directory=NO;
   if(![path isAbsolutePath]||![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&directory]||!directory)
    throw std::runtime_error("FSR Metal asset root must be an existing absolute directory");
   return path;
  }
  Dl_info info{};
  if (!dladdr(reinterpret_cast<const void*>(&fsr4AssetPath), &info) || !info.dli_fname)
   throw std::runtime_error("FSR Metal module path unavailable");
  unsigned long size=0;
  const auto* bytes=getsectiondata(static_cast<const mach_header_64*>(info.dli_fbase), "__DATA", "__fsr_assets", &size);
  if(bytes && size){
   NSData* payload=[NSData dataWithBytes:bytes length:size];
   unsigned char digest[CC_SHA256_DIGEST_LENGTH];
   CC_SHA256(bytes, static_cast<CC_LONG>(size), digest);
   NSMutableString* identity=[NSMutableString string];
   for(unsigned i=0;i<sizeof(digest);++i) [identity appendFormat:@"%02x",digest[i]];
   NSString* base=[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Metal FSR4/shared"];
   NSString* target=[[base stringByAppendingPathComponent:identity] stringByAppendingPathComponent:@"4.0.2"];
   NSError* error=nil;
   id decoded=[NSPropertyListSerialization propertyListWithData:payload options:NSPropertyListImmutable format:nil error:&error];
   if(![decoded isKindOfClass:[NSDictionary class]]) throw std::runtime_error("Invalid embedded Metal assets");
   NSDictionary* files=decoded;
   for(NSString* relative in files){
    if(![relative isKindOfClass:[NSString class]] || [relative isAbsolutePath] || [[relative pathComponents] containsObject:@".."] || ![files[relative] isKindOfClass:[NSData class]])
     throw std::runtime_error("Invalid embedded Metal asset entry");
    NSString* path=[target stringByAppendingPathComponent:relative];
    NSData* expected=files[relative];
    if([[NSData dataWithContentsOfFile:path] isEqualToData:expected]) continue;
    if(![[NSFileManager defaultManager] createDirectoryAtPath:[path stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:&error] ||
       ![expected writeToFile:path options:NSDataWritingAtomic error:&error])
     throw std::runtime_error("Cannot extract embedded Metal assets");
   }
   return target;
  }
  NSString* module = [[NSString stringWithUTF8String:info.dli_fname] stringByStandardizingPath];
  if (![module isAbsolutePath]) throw std::runtime_error("FSR Metal module path must be absolute");
  NSString* base = [[module stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
  return [base stringByAppendingPathComponent:@"4.0.2"];
 }();
 return [[root stringByAppendingPathComponent:category] stringByAppendingPathComponent:name];
}
