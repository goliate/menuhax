#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <3ds.h>

#include "archive.h"
#include "log.h"

#include "modules_common.h"

Result sdiconhax_install(char *menuhax_basefn);
Result sdiconhax_delete();

void register_module_sdiconhax()
{
	module_entry module = {
		.name = "sdiconhax",
		.haxinstall = sdiconhax_install,
		.haxdelete = sdiconhax_delete,
		.themeflag = true
	};

	register_module(&module);
}

Result sdiconhax_load_savedatadat(void)
{
	Result ret=0;
	u32 filesize=0;

	memset(filebuffer, 0, filebuffer_maxsize);

	ret = archive_getfilesize(HomeMenu_Extdata, "/SaveData.dat", &filesize);
	if(ret!=0)
	{
		log_printf(LOGTAR_LOG, "Failed to get filesize for extdata SaveData.dat: 0x%08x\n", (unsigned int)ret);
		return ret;
	}

	if(filesize > filebuffer_maxsize)filesize = filebuffer_maxsize;

	if(filesize != 0x2da0)
	{
		log_printf(LOGTAR_ALL, "The SaveData.dat filesize is invalid. This may mean you're running menuhax_manager on an unsupported system-version.\n");
		log_printf(LOGTAR_LOG, "Filesize = 0x%x, expected 0x2da0.\n", (unsigned int)filesize);
		return -2;
	}

	ret = archive_readfile(HomeMenu_Extdata, "/SaveData.dat", filebuffer, filesize);
	if(ret!=0)
	{
		log_printf(LOGTAR_LOG, "Failed to read SaveData.dat: 0x%08x\n", (unsigned int)ret);
		return ret;
	}

	if(filebuffer[0]<2 || filebuffer[0]>4)
	{
		log_printf(LOGTAR_ALL, "The SaveData.dat format is unsupported. This may mean you're running menuhax_manager on an unsupported system-version.\n");
		log_printf(LOGTAR_LOG, "Format = 0x%x, expected 2..4.\n", (unsigned int)filebuffer[0]);
		return -2;
	}

	return ret;
}

Result sdiconhax_locatelinearmem(u32 *outaddr0, u32 *outaddr1, s16 *icon_16val, u32 *original_objptrs)
{
	u32 *tmpbuf;
	u32 *linearaddr;
	u32 linearpos = osGetMemRegionSize(MEMREGION_APPLICATION);
	u32 chunksize=0x800000;
	u32 pos;
	u32 tmpval;
	u32 iconbuffer_pos;

	//Copy the first 8MB of the SYSTEM memregion into tmpbuf.

	tmpbuf = linearAlloc(chunksize);
	if(tmpbuf==NULL)
	{
		log_printf(LOGTAR_LOG, "Failed to allocate mem for tmpbuf.\n");
		return -1;
	}

	linearaddr = (u32*)(0x30000000+linearpos);

	memset(tmpbuf, 0, chunksize);
	GSPGPU_FlushDataCache(tmpbuf, chunksize);

	GX_TextureCopy(linearaddr, 0, tmpbuf, 0, chunksize, 0x8);
	gspWaitForPPF();

	for(pos=0; pos<(chunksize-8)>>2; pos++)//Locate the address of the SaveData.dat buffer in the linearmem heap, since it varies per system in some cases.
	{
		if(tmpbuf[pos]==0x5544 && tmpbuf[pos+1]==0x2da0)//Check the CTRSDK heap memchunkhdr.
		{
			break;
		}
	}

	if(pos==((chunksize-8)>>2))
	{
		linearFree(tmpbuf);
		return -2;
	}

	pos+= 4;

	*outaddr0 = (u32)&linearaddr[pos];//Actual address of the SaveData.dat buffer.

	iconbuffer_pos = pos + (0x2da0>>2);

	if(!(tmpbuf[iconbuffer_pos]==0x5544 && tmpbuf[iconbuffer_pos+1]==0x7bc0))//Check the CTRSDK heap memchunkhdr.
	{
		log_printf(LOGTAR_LOG, "The icon buffer was not found at the expected address.\n");
		return -3;
	}

	iconbuffer_pos+= 4;

	pos-= 8;

	//Locate the buffer containing the target objects-list.

	while(pos>0)
	{
		if(tmpbuf[pos]==0xc005544 && tmpbuf[pos+1]==0x10)break;

		pos--;
	}

	if(pos==0)
	{
		linearFree(tmpbuf);
		return -2;
	}

	pos+= 4;

	*outaddr1 = (u32)&linearaddr[pos];//Actual address of the target objects-list buffer.

	original_objptrs[0] = tmpbuf[pos];//Original objptrs before they get overwritten.
	original_objptrs[1] = tmpbuf[pos+1];

	tmpval = iconbuffer_pos - pos;

	linearFree(tmpbuf);

	if(tmpval & 1)
	{
		log_printf(LOGTAR_LOG, "The relative offset for iconbuffer_pos->target_objectslist_buffer is 4-byte aligned, 8-byte alignment is required.\n");
		return -4;
	}

	tmpval*= 4;
	*icon_16val = -(tmpval/8);

	log_printf(LOGTAR_LOG, "outaddr0=0x%08x, outaddr1=0x%08x, iconbuffer_pos=0x%x, pos=0x%x, tmpval=0x%x, icon_16val=0x%x\n", *outaddr0, *outaddr1, iconbuffer_pos, pos, tmpval, *icon_16val);

	return 0;
}

Result sdiconhax_install(char *menuhax_basefn)
{
	Result ret=0;

	u32 linearaddr_savedatadat=0;
	u32 linearaddr_target_objectslist_buffer=0;
	s16 icon_16val=0;
	u32 original_objptrs[2] = {0};

	u32 *savedatadat = (u32*)filebuffer;
	u32 *tmpbuf = (u32*)&filebuffer[0x2da0];
	u32 pos;
	u64 *tidptr_in, *tidptr_out;
	s16 *ptr16_in, *ptr16_out;
	s8 *ptr8_in, *ptr8_out;

	char filepath[256];

	log_printf(LOGTAR_ALL, "Loading SaveData.dat...\n");
	ret = sdiconhax_load_savedatadat();
	if(ret!=0)return ret;

	log_printf(LOGTAR_ALL, "Locating data in Home Menu linearmem heap...\n");
	ret = sdiconhax_locatelinearmem(&linearaddr_savedatadat, &linearaddr_target_objectslist_buffer, &icon_16val, original_objptrs);
	if(ret!=0)return ret;

	log_printf(LOGTAR_ALL, "Loading exploit file-data + writing into extdata...\n");

	memset(filepath, 0, sizeof(filepath));

	snprintf(filepath, sizeof(filepath)-1, "romfs:/finaloutput/sdiconhax.zip@%s_sdiconhax.bin", menuhax_basefn);

	ret = archive_readfile(SDArchive, filepath, (u8*)tmpbuf, 0x2da0);
	if(ret!=0)
	{
		log_printf(LOGTAR_LOG, "Failed to load data from romfs: 0x%08x\n", (unsigned int)ret);
		return ret;
	}

	tidptr_in = (u64*)&tmpbuf[0x8>>2];
	ptr16_in = (s16*)&tmpbuf[0xcb0>>2];
	ptr8_in = (s8*)&tmpbuf[0xf80>>2];

	tidptr_out = (u64*)&savedatadat[0x8>>2];
	ptr16_out = (s16*)&savedatadat[0xcb0>>2];
	ptr8_out = (s8*)&savedatadat[0xf80>>2];

	//Only copy data to SaveData.dat when the titleID is set.
	for(pos=0; pos<360; pos++)
	{
		if(tidptr_in[pos] != ~0)
		{
			tidptr_out[pos] = tidptr_in[pos];
			if(ptr16_in[pos]!=0x5848)
			{
				ptr16_out[pos] = ptr16_in[pos];
			}
			else
			{
				ptr16_out[pos] = icon_16val;
			}
			ptr8_out[pos] = ptr8_in[pos];
		}
	}

	ret = archive_writefile(HomeMenu_Extdata, "/SaveData.dat", (u8*)savedatadat, 0x2da0, 0);

	return ret;
}

Result sdiconhax_delete()
{
	Result ret=0;
	s16 *ptr;
	u32 pos;
	u32 update = 0;

	log_printf(LOGTAR_ALL, "Loading SaveData.dat...\n");
	ret = sdiconhax_load_savedatadat();
	if(ret!=0)return ret;

	ptr = (s16*)&filebuffer[0xcb0];
	for(pos=0; pos<360; pos++)
	{
		if(ptr[pos] < -1)//The exploit implementation used here only uses negative values for this, so this code only checks for negative values.
		{
			ptr[pos] = -1;
			update = 1;
		}
	}

	if(update)
	{
		log_printf(LOGTAR_ALL, "Writing the updated SaveData.dat...\n");
		ret = archive_writefile(HomeMenu_Extdata, "/SaveData.dat", filebuffer, 0x2da0, 0);
	}
	else
	{
		log_printf(LOGTAR_ALL, "SaveData.dat wasn't updated since sdiconhax wasn't detected.\n");
	}

	return ret;
}
