# MkLevel resolves the level's sprite references against the generated
# $(INC_DIR)/Sprites.h, so the .lvl it writes bakes in whatever frame numbers
# that header held.  The rule below therefore has to depend on it: without
# that, a from-scratch build can run MkLevel before the header exists at all
# (make is free to order the two), and an incremental build never rebuilds a
# level after the sprite bank changes.  Either way the .lvl keeps stale frame
# indices and the level draws the wrong sprites - most visibly as blocks of
# missing geometry in the FMA cutscenes (github issue #43).
$OutFile=shift(@ARGV);
$InStr=shift(@ARGV);

@Tmp=split('_',$InStr);
$Chapter=shift(@Tmp);
$Level=shift(@Tmp);
$LevelDir =$Chapter/\$Level;

# printf("I got\n0: $OutFile\n1: $InStrn\n");
# printf("Chapter  = $Chapter\n");
# printf("Level    = $Level\n");
# printf("LevelDir = $LevelDir\n");

$OutFile=">$OutFile";
open(OutFile) || die "Can't create makefile $OutFile; $!";
print OutFile <<eot
# print  <<eot
.PHONY : make$Chapter\_$Level clean$Chapter\_$Level

make$Chapter\_$Level\:\t$Chapter\_$Level\_LVL\n
clean$Chapter\_$Level\:\tclean$Chapter\_$Level\_LVL

$Chapter\_$Level\_IN  :=\t\$(LEVELS_IN_DIR)/$Chapter/$Level/$Level.mex
$Chapter\_$Level\_OUT :=\t\$(LEVELS_OUT_DIR)/$Chapter\_$Level.lvl
$Chapter\_$Level\_TEX :=\t\$(LEVELS_OUT_DIR)/$Chapter\_$Level.tex

clean$Chapter\_$Level\_LVL :\n\t\$(RM) -f \$($Chapter\_$Level\_OUT) \$($Chapter\_$Level\_TEX)
$Chapter\_$Level\_LVL :\t\$($Chapter\_$Level\_IN)

\$($Chapter\_$Level\_OUT) : \$($Chapter\_$Level\_IN) \$(INC_DIR)/Sprites.h
\t\@\$(MKLEVEL) \$($Chapter\_$Level\_IN) -o:\$($Chapter\_$Level\_OUT) -i:\$(INC_DIR) \$(LEVELS_OPTS) \$($Chapter\_$Level\_OPTS)

eot
;
close(OutFile);
