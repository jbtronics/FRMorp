#
# Makefile
#
#

TARGET  = frmorp
CC = gcc

#$(TARGET):
#       $(CC) frmorp.c -p $(TARGET)

#all    : $(TARGET)
all:
	$(CC) frmorp.c  -I Win  -o $(TARGET) -L ./Win/ -l usb
clean:
	rm -f $(TARGET)


