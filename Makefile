# loophole-guest's user-mode half for x64 and x86, cross-compiled with MinGW-w64 through
# meson/<arch>.ini. installer/package.proj builds the rest on Windows.
#
#   make                  both architectures, into out/x64 and out/x86
#   make x64, make x86    one of them
#   make iso              out/loophole-guest.iso, the disc around out/package/loophole-setup.exe
#
# Each part configures once: delete build/<arch>/<part> after changing its options here.

SHELL := bash
.SHELLFLAGS := -euo pipefail -c
.ONESHELL:
.DELETE_ON_ERROR:

ARCHS := x64 x86
parts := mesa-radv mesa-zink dxvk vkd3d-proton umd
triple.x64 := x86_64-w64-mingw32
triple.x86 := i686-w64-mingw32

all: $(ARCHS)

.PHONY: all iso clean $(ARCHS) $(foreach a,$(ARCHS),$(addprefix $a-,$(parts)))

third_party/%/meson.build:
	git submodule update --init --recursive --depth 1 third_party/$*

# The WDK headers of the escape ABI and the UMD DDIs, which MinGW lacks or has older copies of.
wdk-version := 10.0.26100.6584
wdk-include := c/Include/10.0.26100.0
sdk-cpp-sha256 := 5d31b38205bdd9ac761b4cb39fbbc6b7209b01c11194324afc674d7d119483a0
wdk-x64-sha256 := c393d03dfb640b5c92f546b32f6770ef68cd3aaf691956e7d66d8e2c28a1b55e

build/wdk/.stamp:
	@rm -rf build/wdk
	mkdir -p build/wdk/include
	for p in sdk.cpp:$(sdk-cpp-sha256) wdk.x64:$(wdk-x64-sha256); do
	    name=microsoft.windows.$${p%%:*}
	    curl -fsSL -o build/wdk/$$name.zip https://api.nuget.org/v3-flatcontainer/$$name/$(wdk-version)/$$name.$(wdk-version).nupkg
	    echo "$${p#*:}  build/wdk/$$name.zip" | sha256sum -c --quiet
	done
	unzip -q -j -d build/wdk/include build/wdk/microsoft.windows.sdk.cpp.zip \
	    $(wdk-include)/shared/{d3dkmthk,d3dkmdt,d3dukmdt}.h $(wdk-include)/um/{dxgiddi,dxmini}.h
	unzip -q -j -d build/wdk/include build/wdk/microsoft.windows.wdk.x64.zip \
	    $(wdk-include)/um/{d3d10umddi.h,d3d11TokenizedProgramFormat.hpp,d3d12umddi.h,d3dumddi.h} \
	    $(wdk-include)/shared/d3dkmddi.h $(wdk-include)/km/wmidata.h
	rm build/wdk/*.zip
	touch $@

# LunarG's Vulkan loader, for the per-game layers.
vulkan-runtime-version := 1.4.357.0
vulkan-runtime-sha256 := a14672efed15aafc7f5a16572d35cd3a3416eadf670aeee3cdf50ee32d5fbf83

build/vulkan-runtime/.stamp:
	@rm -rf build/vulkan-runtime
	mkdir -p build/vulkan-runtime
	zip=build/vulkan-runtime/components.zip
	curl -fsSL -o $$zip https://sdk.lunarg.com/sdk/download/$(vulkan-runtime-version)/windows/vulkan-runtime-components.zip
	echo "$(vulkan-runtime-sha256)  $$zip" | sha256sum -c --quiet
	unzip -q $$zip -d build/vulkan-runtime/unpacked
	mv build/vulkan-runtime/unpacked/*/* build/vulkan-runtime/
	rm -r $$zip build/vulkan-runtime/unpacked
	touch $@

# $(call meson,<arch>,<part>,<source>,<option>...)
define meson
echo "== $1 $2"
[[ -f build/$1/$2/build.ninja ]] ||
    meson setup --cross-file $(CURDIR)/meson/$1.ini --prefix=$(CURDIR)/build/$1/prefix/$2 \
        --buildtype=release -Dstrip=true $4 build/$1/$2 $3
meson compile -C build/$1/$2
meson install -C build/$1/$2 --no-rebuild --quiet
endef

wdk-args = -isystem $(CURDIR)/build/wdk/include
mesa-options := -Dplatforms=windows -Dvulkan-layers= -Dllvm=disabled -Dgles1=disabled -Dgles2=disabled \
	-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dvideo-codecs= -Dzstd=disabled -Dbuild-tests=false \
	-Dmin-windows-version=10
# PE images have no GNU build-id, which RADV hashes into its pipeline cache UUID.
radv-build-id = $(shell git -C third_party/mesa rev-parse HEAD)

$(ARCHS:=-mesa-radv): %-mesa-radv: third_party/mesa/meson.build build/wdk/.stamp
	@$(call meson,$*,mesa-radv,third_party/mesa,$(mesa-options) -Dgallium-drivers= -Dvulkan-drivers=amd \
	    -Damdgpu-virtio=true -Dopengl=false -Dshader-cache=disabled -Dradv-build-id=$(radv-build-id) \
	    '-Dc_args=$(wdk-args) -I$(CURDIR)/abi' '-Dcpp_args=$(wdk-args) -I$(CURDIR)/abi')

$(ARCHS:=-mesa-zink): %-mesa-zink: third_party/mesa/meson.build
	@$(call meson,$*,mesa-zink,third_party/mesa,$(mesa-options) -Dgallium-drivers=zink -Dvulkan-drivers= \
	    -Dopengl=true)

$(ARCHS:=-dxvk): %-dxvk: third_party/dxvk/meson.build
	@$(call meson,$*,dxvk,third_party/dxvk)

$(ARCHS:=-vkd3d-proton): %-vkd3d-proton: third_party/vkd3d-proton/meson.build
	@$(call meson,$*,vkd3d-proton,third_party/vkd3d-proton,-Denable_tests=false)

$(ARCHS:=-umd): %-umd: build/wdk/.stamp
	@$(call meson,$*,umd,umd,'-Dc_args=$(wdk-args)')

setup-flags = -municode -O2 -Wall -Wextra -Wno-missing-field-initializers -Werror -static $(wdk-args)

out/x64/setup/loophole.exe: installer/setup.c build/wdk/.stamp
	@mkdir -p $(@D)
	$(triple.x64)-gcc $(setup-flags) -o $@ $< -lsetupapi -lcfgmgr32 -lcrypt32 -ladvapi32 -luser32 -lshell32

out/x64/setup/loophole-agent.exe: installer/agent.c abi/viogpum.h build/wdk/.stamp
	@mkdir -p $(@D)
	$(triple.x64)-gcc $(setup-flags) -mwindows -Iabi -o $@ $< -lgdi32 -luser32 -lshell32

x64: out/x64/setup/loophole.exe out/x64/setup/loophole-agent.exe

# $(call collect,<arch>,<part>[,<file>]): the part's DLLs, or just <file>, into out/<arch>/<part>.
define collect
mkdir -p out/$1/$2
cp build/$1/prefix/$2/bin/$(or $3,*.dll) out/$1/$2/
endef

$(ARCHS): %: $(addprefix %-,$(parts)) build/vulkan-runtime/.stamp
	@rm -rf $(addprefix out/$*/,$(parts) vulkan-loader)
	$(call collect,$*,mesa-radv,vulkan_radeon.dll)
	$(call collect,$*,mesa-zink)
	$(call collect,$*,dxvk)
	$(call collect,$*,vkd3d-proton)
	$(call collect,$*,umd,loophole_umd.dll)
	mkdir -p out/$*/vulkan-loader
	cp build/vulkan-runtime/$*/vulkan-1.dll build/vulkan-runtime/VulkanRT-License.txt out/$*/vulkan-loader/
	# Whatever imports a MinGW runtime DLL would fail to load in the guest.
	bad=0
	while IFS= read -r -d '' f; do
	    while read -r dll; do
	        case $${dll,,} in
	        libgallium_wgl.dll) ;;
	        lib*.dll | zlib1.dll | mcfgthread*.dll)
	            echo "$${f#out/} imports $$dll" >&2
	            bad=1
	            ;;
	        esac
	    done < <($(triple.$*)-objdump -p "$$f" | sed -n 's/^[[:space:]]*DLL Name: //p')
	done < <(find out/$* -type f \( -name '*.dll' -o -name '*.exe' \) -print0)
	if ((bad)); then
	    echo "link those statically" >&2
	    exit 1
	fi
	echo "== $* done: out/$*"

iso: out/loophole-guest.iso

out/loophole-guest.iso: out/package/loophole-setup.exe installer/disc-readme.txt
	xorriso -as mkisofs -quiet -V LOOPHOLE -J -joliet-long -r -o $@ -graft-points \
	    loophole-setup.exe=$< README.txt=installer/disc-readme.txt

clean:
	rm -rf $(addprefix build/,$(ARCHS)) out
