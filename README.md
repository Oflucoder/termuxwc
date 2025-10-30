Termuxwc, Wayland compositor for Rootless devices.
aarch64 GLES2 support. 
Unintended Any gnu/linux device support. e.g= your e220s thinkpad that barely lives, running any linux. dont run void though, it doesnt have vulkan things.


Vulkan support.


Graphic accel support if u have root access or u are on pc :p 



I am just starting to code, to port. i will probably have to edit wlroots by majority. even, for the pc. its mmissing some stuff.


theres huge integration missing in wlr/wlr_shm.c and wlr/wlr_renderer.c, i need to apply patches. but every patch brings another ninja error.

I need to struggle lots

The current build fails with call to undeclared function and no member named errors. 

i will learn through it.. however. i will also rebuild this for pc in another completely unsketchy name for shits and giggles.


steps to do:
  1.fix shared memory
  2.fix the src/main.c, add necessary placeholders //optimize
  3.apply heavy patches to wlroots, e.g wlr/wlr_shm.c ~ 519 -> assert(hasasrgb... )
  4.write needed headers and .c files
  4.integrate shared memory workarounds
  5.launch a terminal for gods sake.

if u try to run on pc, gives: termuxwc: types/wlr_shm.c:517: wlr_shm_create: Assertion `has_argb8888 && has_xrgb8888' failed.
on phone, the git version works but locally im having errors have fun. and please wish me luck

Build tutorial (
clone wlroots
git clone https://gitlab.freedesktop.org/wlroots/wlroots.git







im stuck in a debugging loop right now, sorry. im still fighting, pathing -> build error fixing. ❎ 
fixed. trying diffrent workarounds to make the terminal apear.





... only x works. wayland gives renderer and memory errors.. its irrecoverable. i will look back into this. for now i willl make a x11 based one instead. i will return dont worry 




github repository for my attempt for developing a wayland compositor. u are free to contribute. much love <3
# termuxwc

