# Builds the application icon into \resource\apps\ocgui_aif.mbm.
#
# The bitmaps come from tools\mkicon.js and are committed under src\gfx, so an
# ordinary build needs no node: this makefile only packs them. Three sizes are
# packed, each drawn separately - see the comment at the top of mkicon.js for
# why they are not one bitmap scaled down.
#
# Sources are BMP rather than SVG on purpose. SVG Tiny 1.1, which is what the
# S60 3rd FP1 renderer takes, has no gradients at all, and the dial is a
# gradient. With BMP input mifconv produces an .mbm, not a .mif; both are valid
# targets for icon_file.

ifeq (WINS,$(findstring WINS, $(PLATFORM)))
ZDIR=$(EPOCROOT)epoc32\release\$(PLATFORM)\$(CFG)\Z
else
ZDIR=$(EPOCROOT)epoc32\data\z
endif

TARGETDIR=$(ZDIR)\resource\apps
ICONTARGETFILENAME=$(TARGETDIR)\ocgui_aif.mbm

ICONDIR=..\src\gfx

do_nothing :
	@rem do_nothing

MAKMAKE : do_nothing

BLD : do_nothing

CLEAN :
	@if exist "$(ICONTARGETFILENAME)" del "$(ICONTARGETFILENAME)"

LIB : do_nothing

CLEANLIB : do_nothing

RESOURCE :
	mifconv $(TARGETDIR)\ocgui_aif.mif \
		/c24,8 $(ICONDIR)\ocgui_88.bmp \
		/c24,8 $(ICONDIR)\ocgui_44.bmp \
		/c24,8 $(ICONDIR)\ocgui_22.bmp

FREEZE : do_nothing

SAVESPACE : do_nothing

RELEASABLES :
	@echo $(ICONTARGETFILENAME)

FINAL : do_nothing
