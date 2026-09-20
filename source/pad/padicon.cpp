/*=========================================================================

	padicon.cpp

	Author:		SBSPSS Win11 port
	Created:
	Project:	Spongebob
	Purpose:	One resolver for "which icon goes beside this prompt".

	Copyright (c) 2000 Climax Development Ltd

===========================================================================*/


/*----------------------------------------------------------------------
	Includes
	-------- */

#include "pad\padicon.h"

#ifndef __SYSTEM_ASMPORT_H__
#include "system\asmport.h"
#endif


/*	Data
	---- */

#ifndef __SPR_SPRITES_H__
#include <sprites.h>
#endif


/*----------------------------------------------------------------------
	Vars
	---- */

/*	The eight PS1 glyphs, and the name the shim knows each button by.  The
	names are the key_* spellings from sbsp.ini (port/psyq/host/input.cpp
	g_keys[]) - the only place a binding lives.

	FRM__BUTL / FRM__BUTR are the D-pad's left and right arrows, not the
	shoulder buttons; the game has no L1/R1/Start/Select glyph, and none of
	the prompt sites ask for one.
*/
static const struct
{
	int			m_padButton;
	const char	*m_name;
	int			m_frame;
} s_buttons[]=
{
	{	PAD_UP,			"up",		FRM__BUTU	},
	{	PAD_DOWN,		"down",		FRM__BUTD	},
	{	PAD_LEFT,		"left",		FRM__BUTL	},
	{	PAD_RIGHT,		"right",	FRM__BUTR	},
	{	PAD_CROSS,		"cross",	FRM__BUTX	},
	{	PAD_CIRCLE,		"circle",	FRM__BUTC	},
	{	PAD_SQUARE,		"square",	FRM__BUTS	},
	{	PAD_TRIANGLE,	"triangle",	FRM__BUTT	},
};
#define	NUM_BUTTONS		(int)(sizeof(s_buttons)/sizeof(s_buttons[0]))


#ifndef	PSX_MIPS_ASM
/*	Key caps, indexed by the PORT_CAP_* the shim returns.  The order is the
	enum's order in system/asmport.h - the array check below keeps the two
	from drifting apart, but it cannot check the order, so edit both
	together.  PORT_CAP_NONE is the "draw the PS1 glyph" answer.
*/
static const int	s_capFrame[]=
{
	-1,					/* PORT_CAP_NONE	*/
	FRM__KEYA,			/* PORT_CAP_A		*/
	FRM__KEYS,			/* PORT_CAP_S		*/
	FRM__KEYX,			/* PORT_CAP_X		*/
	FRM__KEYZ,			/* PORT_CAP_Z		*/
	FRM__KEYQ,			/* PORT_CAP_Q		*/
	FRM__KEYW,			/* PORT_CAP_W		*/
	FRM__KEYE,			/* PORT_CAP_E		*/
	FRM__KEYR,			/* PORT_CAP_R		*/
	FRM__KEYUP,			/* PORT_CAP_UP		*/
	FRM__KEYDOWN,		/* PORT_CAP_DOWN	*/
	FRM__KEYLEFT,		/* PORT_CAP_LEFT	*/
	FRM__KEYRIGHT,		/* PORT_CAP_RIGHT	*/
	FRM__KEYENTER,		/* PORT_CAP_ENTER	*/
	FRM__KEYRSHIFT,		/* PORT_CAP_RSHIFT	*/
};
typedef char	s_capFrameIsComplete[(sizeof(s_capFrame)/sizeof(s_capFrame[0]))==PORT_CAP__COUNT?1:-1];
#endif


/*----------------------------------------------------------------------
	Function:	CPadIcon::getPsxFrame
	Purpose:	The PS1 glyph for a pad button.
	Params:		_padButton - a single PAD_* mask
	Returns:	a frame in Sprites.Spr
  ---------------------------------------------------------------------- */
int	CPadIcon::getPsxFrame(int _padButton)
{
	for(int i=0;i<NUM_BUTTONS;i++)
	{
		if(s_buttons[i].m_padButton==_padButton)
		{
			return s_buttons[i].m_frame;
		}
	}
	/*	The original code ASSERTed here and then drew whatever was in the
		uninitialised icon slot.  Every caller passes a literal or a
		CPadConfig entry, so this is unreachable short of a new pad
		config; the cross is the one glyph that is never wrong enough to
		be worth a crash in a release build.  */
	ASSERT(!"Unknown Pad Button");
	return FRM__BUTX;
}


/*----------------------------------------------------------------------
	Function:	CPadIcon::getFrame
	Purpose:	The icon to draw beside a prompt for a pad button - the
				key cap the player is actually pressing where we know it,
				the PS1 glyph otherwise.
	Params:		_padButton - a single PAD_* mask
	Returns:	a frame in Sprites.Spr
  ---------------------------------------------------------------------- */
int	CPadIcon::getFrame(int _padButton)
{
#ifndef	PSX_MIPS_ASM
	for(int i=0;i<NUM_BUTTONS;i++)
	{
		if(s_buttons[i].m_padButton==_padButton)
		{
			int	cap=Port_InputPromptCap(s_buttons[i].m_name);
			if(cap>PORT_CAP_NONE&&cap<PORT_CAP__COUNT&&s_capFrame[cap]!=-1)
			{
				return s_capFrame[cap];
			}
			return s_buttons[i].m_frame;
		}
	}
#endif
	return getPsxFrame(_padButton);
}


/*----------------------------------------------------------------------
	Function:	CPadIcon::getUpDownFrame
	Purpose:	The single sprite for the Up+Down prompt slot, where the
				key-cap set has one.
	Params:
	Returns:	a frame in Sprites.Spr, or -1 for "draw the two icons"
  ---------------------------------------------------------------------- */
int	CPadIcon::getUpDownFrame()
{
#ifndef	PSX_MIPS_ASM
	/*	Only when BOTH halves resolved to their own key caps: if either is
		still a PS1 glyph - a gamepad is driving, or one of the two has
		been rebound to a key with no cap - the pair has to be drawn from
		the two separate icons or it would mix the two icon sets.  */
	if(getFrame(PAD_UP)!=getPsxFrame(PAD_UP)&&
	   getFrame(PAD_DOWN)!=getPsxFrame(PAD_DOWN))
	{
		return FRM__KEYUPDOWN;
	}
#endif
	return -1;
}


/*===========================================================================
 end */
