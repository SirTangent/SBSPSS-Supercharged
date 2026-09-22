/*=========================================================================

	padicon.h

	Author:		SBSPSS Win11 port
	Created:
	Project:	Spongebob
	Purpose:	One resolver for "which icon goes beside this prompt".

	Copyright (c) 2000 Climax Development Ltd

===========================================================================*/

#ifndef	__PAD_PADICON_H__
#define	__PAD_PADICON_H__

/*----------------------------------------------------------------------
	Includes
	-------- */

#ifndef __PAD_PADS_H__
#include "pad\pads.h"
#endif


/*----------------------------------------------------------------------
	Structure defintions
	-------------------- */

/*	Every button prompt in the game - the in-game item prompts, the
	frontend footers, the map/shop/slot-select instructions, the dialogue
	and boss text boxes - draws a small icon for the pad button it is
	talking about.  On the PlayStation that is always the PS1 glyph.  On PC
	the player is usually on the keyboard, where the glyph tells them
	nothing: they have to already know that [] means A before the prompt
	means anything (github issue #43).

	Every one of those sites now asks here for its frame instead of naming
	FRM__BUT* directly, so the answer can depend on what the player is
	actually holding.  The PS1 glyphs are still the answer whenever a
	gamepad is the active device - a pad player keeps the icons that match
	the pad in their hands - and whenever there is no cap art for the key a
	button has been rebound to.
*/
class	CPadIcon
{
public:
	/*	_padButton is a single PAD_* mask (PAD_CROSS, PAD_UP, ...), which
		is what CPadConfig::getButton() returns.  The answer is a frame in
		the Sprites.Spr bank, suitable for SpriteBank::printFT4 or
		::getFrameHeader.  Unknown buttons return the cross glyph rather
		than an out-of-range frame.  */
	static int		getFrame(int _padButton);

	/*	The same thing starting from a logical action, for the sites that
		hold a CPadConfig::PAD_CFG (the in-game prompt table, the options
		control readout).  */
	static int		getFrameForCfg(CPadConfig::PAD_CFG _cfg)
					{ return getFrame(CPadConfig::getButton(_cfg)); }

	/*	The PS1 glyph for a button, ignoring the keyboard entirely.  For
		the places that want the hardware icon as such.  */
	static int		getPsxFrame(int _padButton);

	/*	Every prompt site sits its icon a few pixels below its text, by an
		offset that was tuned by eye for the 11px PS1 glyph.  The 14px key
		caps want their own: not one correction for all of them, because
		each screen's font, outline and leading put the text somewhere
		different, so the caps were tuned screen by screen just as the
		glyphs were.  A site keeps both offsets side by side and asks here
		which applies to the icon it is about to draw.  The glyph offset is
		the answer for a glyph, so the PS1 build and a gamepad player see
		no change.  */
	static int		getYOffset(int _padButton,int _glyphYOffset,int _capYOffset)
					{ return getFrameYOffset(getFrame(_padButton),_glyphYOffset,_capYOffset); }
	static int		getFrameYOffset(int _frame,int _glyphYOffset,int _capYOffset)
					{ return isKeyCap(_frame)?_capYOffset:_glyphYOffset; }

	/*	Whether a frame getFrame() answered with is one of the key caps
		rather than a PS1 glyph.  */
	static int		isKeyCap(int _frame);
};


/*---------------------------------------------------------------------- */

#endif	/* __PAD_PADICON_H__ */

/*===========================================================================
 end */
