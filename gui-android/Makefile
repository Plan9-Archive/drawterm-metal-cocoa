ROOT=..
include ../Make.config
LIB=libgui.a

OFILES=\
	cpp/android.$O\
	cpp/native-lib.$O\
	cpp/devandroid.$O\

default: $(LIB) gen/org/echoline/drawterm/R.java bin/classes.dex
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

gen/org/echoline/drawterm/R.java: $(shell find res/ -type f)
	$(SDKPREFIX)/build-tools/30.0.3/aapt package -f -m -M AndroidManifest.xml -I $(SDKPREFIX)/platforms/android-21/android.jar -S res/ -J gen

bin/classes.dex: obj/org/echoline/drawterm/MainActivity.class obj/org/echoline/drawterm/DrawTermThread.class obj/org/echoline/drawterm/MySurfaceView.class
	$(SDKPREFIX)/build-tools/30.0.3/dx --dex --verbose --output=$@ obj/

obj/org/echoline/drawterm/%.class: java/org/echoline/drawterm/%.java
	$(JAVA_HOME)/bin/javac -d obj/ -classpath $(SDKPREFIX)/platforms/android-21/android.jar -sourcepath java java/org/echoline/drawterm/$*.java gen/org/echoline/drawterm/R.java

