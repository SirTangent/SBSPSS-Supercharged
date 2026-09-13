/****************/
/*** PSX Main ***/
/****************/

#include <libmcrd.h>

#include 	"system\global.h"
#include 	"fileio\fileio.h"
#include 	"pad\pads.h"
#include 	"pad\vibe.h"
#include 	"system\vid.h"
#include 	"gfx\prim.h"
#include 	"gfx\tpage.h"
#include	"utils\utils.h"
#include	"gfx\actor.h"

#include 	"system\gp.h"

// scenes
#include "game\game.h"

#ifndef __FRONTEND_FRONTEND_H__
#include "frontend\frontend.h"
#endif

#ifdef __USER_paul__
#include "paul\paul.h"
CPaulScene s_paulScene;
#endif

#if defined (__USER_paul__)||defined (__USER_art__)||defined (__USER_sbart__)
#include "paul\scenesel.h"
#endif

#ifdef __USER_charles__
#include "map\map.h"
#endif


#ifndef __SYSTEM_GSTATE_H__
#include "system\gstate.h"
#endif

#ifndef __LOCALE_TEXTDBASE_H__
#include "locale\textdbase.h"
#endif

#ifndef	__SOUND_SOUND_H__
#include "sound\sound.h"
#endif


#ifndef __SYSTEM_EXCEPT_H__
#include "system\except.h"
#endif

#ifndef __GFX_FONT_H__
#include "gfx\font.h"
#endif

#ifndef __GUI_GUI_H__
#include "gui\gui.h"
#endif

#ifndef	__GAME_GAMESLOT_H__
#include "game\gameslot.h"
#endif

#ifndef	__GFX_FADER_H__
#include "gfx\fader.h"
#endif

#ifndef __GFX_BUBICLES_H__
#include "gfx\bubicles.h"
#endif

#include	<sprites.h>

#ifndef __MEMCARD_MEMCARD_H__
#include "memcard\memcard.h"
#endif

#ifndef __MEMCARD_SAVELOAD_H__
#include "memcard\saveload.h"
#endif


/*	PC port: the screen utils (VRamViewer/SaveScreen) are debug tools worth
	keeping on any host with a real filesystem - the Win32 shim implements
	the libsn PC* file calls they need.  The extra !PSX_MIPS_ASM arms keep
	the PlayStation build exactly as before (asmport.h defines PSX_MIPS_ASM
	there, so both conditions reduce to the originals).  */
#if	__FILE_SYSTEM__==PC || !defined(PSX_MIPS_ASM)
	#if	!defined(__USER_CDBUILD__) || !defined(PSX_MIPS_ASM)
		#if defined(__VERSION_DEBUG__)
			#define	USE_SCREEN_UTILS
		#endif
	#endif
#endif


/*****************************************************************************/
static SpriteBank	GenericSpriteBank;

/*****************************************************************************/

void	SaveScreen(RECT R);

/*****************************************************************************/
static void DoAutoLoad()
{
	MemCard::Start();
	CSaveLoadDatabase	autoloadDb;

	autoloadDb.startAutoload();

	while(1)
	{
		MemCard::Handler();
		autoloadDb.think();
		if(!autoloadDb.monitorAutoload())
			break;
		VSync(0);
	}

	MemCard::Stop();
}

#if	!defined(PSX_MIPS_ASM)
/*	PC port: boot-time FULL load of the memory card (settings AND game
	slots), so the slot-select screen shows saved games without a manual
	Options -> Load Game every launch.  The retail autoload path above is
	no use for that even if re-enabled: startAutoload's completion calls
	restoreData(settings-only) and it waits a fixed 2 seconds for a
	physical card to settle.  This variant polls the card to ValidCard
	(the shim's card settles in a handful of frames; the 120-frame cap
	covers an unusable save location) and drives the ordinary startLoad
	path, whose completion restores everything after the MD5 check.
	A missing/empty/unformatted card just falls through - no UI, no
	format offer; the in-game screens still own the error paths.  */
static void DoAutoLoadPC()
{
	MemCard::Start();
	CSaveLoadDatabase	autoloadDb;
	int					frames;

	for(frames=0;frames<120;frames++)
	{
		autoloadDb.think();
		if(MemCard::GetCardStatus(0)==MemCard::CS_ValidCard)
			break;
		VSync(0);
	}

	if(MemCard::GetCardStatus(0)==MemCard::CS_ValidCard&&
	   MemCard::GetFileCountOnCard(0)&&
	   autoloadDb.startLoad(0))
	{
		/*	Bounded like the poll above: getLoadStatus only leaves
			IN_PROGRESS when a completion callback fires (or the card
			reports removal), and memcard.cpp has paths that drop back to
			CmdNone without calling it.  Boot must not hang on a black
			screen if one is ever reached - give up and start unloaded.  */
		for(frames=0;frames<120;frames++)
		{
			if(autoloadDb.getLoadStatus()!=CSaveLoadDatabase::IN_PROGRESS)
				break;
			autoloadDb.think();
			VSync(0);
		}
	}

	MemCard::Stop();
}
#endif

/*****************************************************************************/
void	InitSystem()	// reordered to reduce black screen (hope all is well
{
	ResetCallback();
	SaveGP();
	SetSp(GetSp()|0x807f0000);
//	SetDispMask(0);

	MemInit();
	MemCardInit( 1 );
	MemCardStart();
	PadsInit();
	MemCardStop();
	CPadVibrationManager::init();

	CFileIO::Init();
	TranslationDatabase::initialise(false);
	TranslationDatabase::loadLanguage(ENGLISH);
	PrimInit();
	TPInit();
	VidInit();

#ifdef __USER_paul__
	installExceptionHandler();			// Where is the earliest we can do this?
#endif

#if	!defined(PSX_MIPS_ASM)
	{
		long	seed;		// PC: --seed / SBSP_SEED (conv_pc.md #26)
		setRndSeed( Port_BootSeed(&seed) ? seed : VidGetTickCount() );
	}
#else
	setRndSeed( VidGetTickCount() );
#endif

	SetDispMask(1);

	GenericSpriteBank.load(SPRITES_SPRITES_SPR);
	CGameScene::setSpriteBank(&GenericSpriteBank);
	SetUpLoadIcon(GenericSpriteBank.getFrameHeader(FRM__TOKEN));
	StartLoad();

	GameState::initialise();
	CSoundMediator::initialise();
	CSoundMediator::setSfxBank(CSoundMediator::SFX_INGAME);
	initGUIStuff();
	CGameSlotManager::init();

	CBubicleFactory::init();

	CActorPool::AddActor(ACTORS_SPONGEBOB_SBK);
	StopLoad();

	// Autoload? Who wants that in this day and age!? Pah! Autoload.. schmautoload!
//#if defined(__USER_paul__) || defined(__USER_CDBUILD__)
//	DoAutoLoad();
//#endif

	/*	PC port only (the PlayStation build defines PSX_MIPS_ASM and keeps
		the retail no-autoload behaviour above): load card0.mcd from
		SBSP_SAVE_DIR / %APPDATA%\SBSPSS at boot - see DoAutoLoadPC.  */
#if	!defined(PSX_MIPS_ASM)
	DoAutoLoadPC();
#endif

#if defined(__DEBUG_MEM__)
	DebugMemFontInit();
#endif

#ifdef __USER_paul__
s_paulScene.init();
#endif
}

/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/
static int	s_time = 0;
#if defined(__DEBUG_MEM__)
void dumpDebugMem();
void DebugMemFontInit();
#endif

void	MainLoop()
{
	while (1)
	{
		int	frames;

		frames=GameState::getFramesSinceLast();

// System Think
		FontBank::think(frames);
		CSoundMediator::think(frames);

// Think States		
		GameState::think();
		CBubicleFactory::think(frames);
		CFader::think(frames);

#ifdef __USER_paul__
		s_paulScene.think(frames);
#endif

// Render States		
		CFader::render();
		GameState::render();
		CBubicleFactory::render();

#ifdef __USER_paul__
		s_paulScene.render();
#endif

		while(DrawSync(1));
		VidSwapDraw();
		PrimDisplay();

		CPadVibrationManager::think(frames);
		PadUpdate();

		DbgPollHost();

#if defined(__VERSION_DEBUG__)

	#if defined(__DEBUG_MEM__)
		dumpDebugMem();
	#endif

	#if		defined(USE_SCREEN_UTILS)
		if (PadGetHeld(0) & PAD_L2)
			if (PadGetDown(0) & PAD_START) SaveScreen(VidGetScreen()->Draw.clip);
		if (PadGetDown(0) & PAD_SELECT) VRamViewer();
	#endif
#endif
	}
}

/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/
#if	defined(__USER_daveo__)
#include	"fma/fma.h"
int			TestFMA=-1;
#endif

#if	!defined(PSX_MIPS_ASM)
extern "C" int	Port_BootLevel(void);			/* port/psyq/host/args.cpp */
extern int		s_globalLevelSelectThing;		/* level/level.h */
extern int		MemNodeCount;					/* mem/memory.cpp */
extern int		invincibleSponge;				/* player/player.cpp */
#endif

int 	main()
{
#if	!defined(PSX_MIPS_ASM)
	// PC: the shim's watches/[summary]/--invincible read these (conv_pc.md #26)
	Port_RegisterGameGlobals(&MainRam.RamUsed,&MemNodeCount,&invincibleSponge,
							 &CurrPrim,&EndPrim,&PrimListStart,&PrimListEnd);
#endif
	CFileIO::GetAllFilePos();
	InitSystem();

#if defined (__USER_paul__)||defined (__USER_art__)||defined (__USER_sbart__)
	GameState::setNextScene( &SceneSelector );
#elif	defined(__USER_daveo__)
	if (TestFMA==-1)
	{
		GameState::setNextScene( &GameScene );
	}
	else
	{
		FmaScene.selectFma((CFmaScene::FMA_SCRIPT_NUMBER)TestFMA);
		GameState::setNextScene(&FmaScene);

	}
#elif	defined(__USER_charles__)
	GameState::setNextScene( &MapScene );
#else
	/*	PC port: --level / SBSP_BOOT_LEVEL boots straight into a level for
		testing - the shim's Port_BootLevel() returns the LvlTable index, or
		-1 for the normal boot.  Same shape as the __USER_daveo__ dev path
		above.  The PlayStation build defines PSX_MIPS_ASM (asmport.h), so
		it reduces to the original line.  */
	#if	!defined(PSX_MIPS_ASM)
	{
		int		bootLevel=Port_BootLevel();
		if (bootLevel>=0)
		{
			s_globalLevelSelectThing=bootLevel;
			GameState::setNextScene( &GameScene );
		}
		else
			GameState::setNextScene( &FrontEndScene );
	}
	#else
	GameState::setNextScene( &FrontEndScene );
	#endif
#endif

//	CXAStream::Init();			// PKG - Stuck here so that it doesn't affect any startup stuff (7/8/00)
	MainLoop();

	return(0);

}

/*****************************************************************************/
#if	defined(USE_SCREEN_UTILS)
#if defined(__VERSION_DEBUG__)
struct	sTgaHdr
{
	char	id;			   // 0
	char	colmaptype;	   // 1
	char	imagetype;	   // 2
	char	fei[2];		   // 3
	char	cml[2];		   // 5
	char	cmes;		   // 7
	short	xorig;		   // 8
	short	yorig;		   // 10
	short	width;		   // 12
	short	height;		   // 14
	char	depth;		   // 15
	char	imagedesc;	   // 16
};

bool FileExists(char const * Name)
{
	int		FileHnd;

	FileHnd=PCopen((char *)Name,0,0);

	if (FileHnd!=-1)
		{
		PCclose(FileHnd);
		return true;
		}
	else
		return false;
}

void SaveScreen(RECT SR)
{
int				FileHnd;
static	int		ScreenNo=0;
sTgaHdr			FileHdr;
int				W=SR.w;
int				H=SR.h;
char			Filename[32];

		sprintf( Filename, "SBSP%04d.tga", ScreenNo );
		while (FileExists( Filename ) )
			{
			ScreenNo++;
			sprintf( Filename, "SBSP%04d.tga", ScreenNo );
			}

		FileHnd=PCcreat((char *)Filename,0);
		ASSERT(FileHnd != -1);
		
//---------------------------------------------------------------------------
// Header
		memset(&FileHdr,0 ,sizeof(sTgaHdr));
	
		FileHdr.imagetype= 2;  //imagetype
		FileHdr.width = W;
		FileHdr.height= H;
		FileHdr.depth=24;
//		FileHdr.imagedesc=24;

		PCwrite(FileHnd,(char *)&FileHdr,sizeof(sTgaHdr));

//---------------------------------------------------------------------------
// Data
int		x,y;
u16		InBuffer[1024];
u8		OutBuffer[1024*3];

		SR.y+=SR.h;
		SR.h=1;
		for (y=0; y<H; y++)
			{
			SR.y--;
			StoreImage(&SR,(u32*)InBuffer);

			for (x=0; x<W; x++)
				{
				u16	Col;
				u8	R,G,B;
				Col=InBuffer[x];

				R=Col&0x1f;
				G=(Col>>5)&0x1f;
				B=(Col>>10)&0x1f;

				R=R*255/31;
				G=G*255/31;
				B=B*255/31;

				OutBuffer[(x*3)+0]=B;
				OutBuffer[(x*3)+1]=G;
				OutBuffer[(x*3)+2]=R;
				}
			PCwrite(FileHnd,(char *)OutBuffer,W*3);
			}

//---------------------------------------------------------------------------
		PCclose(FileHnd);


}
#endif
#endif

