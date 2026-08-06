---
layout: home
---

Putorana reconstructs the real world into a virtual one: ARCore supplies tracking,
the camera image and a depth map, a TSDF fuses those depth maps into a signed
distance field, marching cubes turns the field into meshes, and Vulkan draws them
over the live camera feed. It is written in C++ for Android and runs on the CPU.

It is named after the [Putorana Plateau](https://en.wikipedia.org/wiki/Putorana_Plateau),
the main flood basalt of the Siberian Traps, because Vulkan projects get volcano
names here.

These are the working notes. They are written after a session rather than during
one, and they keep the wrong turns in, because the wrong turns are the expensive
part to rediscover. The code and the notes on how it all fits together live in
[the repository](https://github.com/dongeronimo/Putorana).
