//
//  crt.h
//  Chocolate Wolfenstein 3D
//
//  Created by fabien sanglard on 2014-08-26.
//
//

#ifndef Chocolate_Wolfenstein_3D_crt_h
#define Chocolate_Wolfenstein_3D_crt_h

#include "SDL/SDL.h"
// Win32
#ifdef _WIN32
#include <WTypes.h>
#include <gl\GL.h>
#elif __linux__
#include <GL/gl.h>
#else
#include <OpenGL/gl.h>
#endif

extern SDL_Color curpal[256];
/*
 * CRT aspect ratio is 4:3, height will be infered.
 */
void CRT_Init(int width);

/*
 *   Trigger the Digital To Analogic convertion
 */
void CRT_DAC(void);


#endif
