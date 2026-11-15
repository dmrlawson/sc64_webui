# Host simulator build: make -C host && host/build/sc64webui-sim sd.img
BUILD := build
CFLAGS := -O2 -g -Wall -Wextra -I../src -I../lib/fatfs
SRCS := main_host.c net_posix.c transport_sim.c \
        ../src/config.c ../src/http.c ../src/log.c ../src/sc64.c ../src/storage.c \
        ../lib/fatfs/ff.c ../lib/fatfs/ffunicode.c

$(BUILD)/sc64webui-sim: $(SRCS) $(BUILD)/index_html.c $(wildcard ../src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(BUILD)/index_html.c

$(BUILD)/index_html.c: ../web/index.html ../web/embed.py
	@mkdir -p $(BUILD)
	python3 ../web/embed.py $< $@ index_html_gz

clean:
	rm -rf $(BUILD)
