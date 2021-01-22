#include "endian.hlsli"
#include "pixel_formats.hlsli"
#include "resolve.hlsli"

RWBuffer<uint2> xe_resolve_dest : register(u0);
ByteAddressBuffer xe_resolve_source : register(t0);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  // 1 thread = 8 pixels.
  // Group height is the same as resolve granularity, Y overflow check not
  // needed.
  [branch] if (xe_thread_id.x >= XeResolveSizeDiv8().x) {
    return;
  }
  uint2 pixel_index = xe_thread_id.xy << uint2(3u, 0u);
  float4 pixels_0123, pixels_4567;
  XeResolveLoad8RedColorsX1(
      xe_resolve_source, XeResolveColorCopySourcePixelAddressInts(pixel_index),
      pixels_0123, pixels_4567);
  // Convert to R8.
  // TODO(Triang3l): Investigate formats 8_A and 8_B.
  xe_resolve_dest[XeResolveDestPixelAddress(pixel_index, 0u) >> 3u] =
      uint2(XePackR8G8B8A8UNorm(pixels_0123), XePackR8G8B8A8UNorm(pixels_4567));
}
