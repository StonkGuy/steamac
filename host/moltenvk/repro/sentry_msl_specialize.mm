// STEAMAC-6A: specialization defaults and overrides survive full-width SpecId remapping.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

static void require(bool value, NSString *message)
{
    if (!value) { fprintf(stderr, "%s\n", message.UTF8String); std::exit(1); }
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        require(argc == 4, @"usage: specialize source.metal shadow-index high-index");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, @"No Metal device");
        NSError *error = nil;
        NSString *source = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&error];
        require(source != nil, error.description);
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_0;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description);
        for (int specialized = 0; specialized < 2; specialized++) {
            MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
            if (specialized) {
                bool shadow = false;
                uint32_t low = 11, edge = 21, high = 123;
                [values setConstantValue:&shadow type:MTLDataTypeBool atIndex:std::strtoul(argv[2], nullptr, 10)];
                [values setConstantValue:&low type:MTLDataTypeUInt atIndex:0];
                [values setConstantValue:&edge type:MTLDataTypeUInt atIndex:65535];
                [values setConstantValue:&high type:MTLDataTypeUInt atIndex:std::strtoul(argv[3], nullptr, 10)];
            }
            id<MTLFunction> function = [library newFunctionWithName:@"main0" constantValues:values error:&error];
            require(function != nil, error.description);
            id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
            require(pipeline != nil, error.description);
            id<MTLBuffer> result = [device newBufferWithLength:7 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
            id<MTLCommandQueue> queue = [device newCommandQueue];
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:result offset:0 atIndex:0];
            [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            require(command.status == MTLCommandBufferStatusCompleted, command.error.description);
            const uint32_t defaults[] = {1, 10, 20, 30, 40, 50, 0x3fc00000};
            const uint32_t overrides[] = {0, 11, 21, 123, 123, 50, 123};
            const auto *expected = specialized ? overrides : defaults;
            const auto *actual = static_cast<const uint32_t *>(result.contents);
            for (int i = 0; i < 7; i++) {
                if (actual[i] != expected[i]) {
                    fprintf(stderr, "specialized=%d word=%d expected=%u actual=%u\n", specialized, i, expected[i], actual[i]);
                    return 1;
                }
            }
            printf("%s: all 7 values match; duplicate defaults, same-ID overrides, mixed-type bitcast, and no-SpecId preserved\n", specialized ? "specialized" : "defaults");
        }
    }
}
