
#include "sf22aswt_structures.h"
#include "Print.h"

namespace SF22ASWT
{
    CODE_LOCATION void sample_header_temp::PrintTo(Print &stream)
    {
        stream.print("Sample Start:"); stream.print(sample_start);
        stream.print("\n, LOOP:"); stream.print(LOOP);
        stream.print("\n, SAMPLE_NOTE:"); stream.print(SAMPLE_NOTE);
        stream.print("\n, CENTS_OFFSET:"); stream.print(CENTS_OFFSET);
        stream.print("\n, LENGTH:"); stream.print(LENGTH);
        stream.print("\n, LENGTH_BITS:"); stream.print(LENGTH_BITS);
        stream.print("\n, SAMPLE_RATE:"); stream.print(SAMPLE_RATE);
        stream.print("\n, LOOP_START:"); stream.print(LOOP_START);
        stream.print("\n, LOOP_END:"); stream.print(LOOP_END);
        stream.print("\n, INIT_ATTENUATION:"); stream.print(INIT_ATTENUATION);

        stream.print("\n, DELAY_ENV:"); stream.print(DELAY_ENV);
        stream.print("\n, ATTACK_ENV:"); stream.print(ATTACK_ENV);
        stream.print("\n, HOLD_ENV:"); stream.print(HOLD_ENV);
        stream.print("\n, DECAY_ENV:"); stream.print(DECAY_ENV);
        stream.print("\n, RELEASE_ENV:"); stream.print(RELEASE_ENV); 
        stream.print("\n, SUSTAIN_FRAC:"); stream.print(SUSTAIN_FRAC);

        stream.print("\n, VIB_DELAY_ENV:"); stream.print(VIB_DELAY_ENV);
        stream.print("\n, VIB_INC_ENV:"); stream.print(VIB_INC_ENV);
        stream.print("\n, VIB_PITCH_INIT:"); stream.print(VIB_PITCH_INIT);
        stream.print("\n, VIB_PITCH_SCND:"); stream.print(VIB_PITCH_SCND);

        stream.print("\n, MOD_DELAY_ENV:"); stream.print(MOD_DELAY_ENV);
        stream.print("\n, MOD_INC_ENV:"); stream.print(MOD_INC_ENV);
        stream.print("\n, MOD_PITCH_INIT:"); stream.print(MOD_PITCH_INIT);
        stream.print("\n, MOD_PITCH_SCND:"); stream.print(MOD_PITCH_SCND);
        stream.print("\n, MOD_AMP_INIT_GAIN:"); stream.print(MOD_AMP_INIT_GAIN);
        stream.print("\n, MOD_AMP_SCND_GAIN:"); stream.print(MOD_AMP_SCND_GAIN);
    }

    CODE_LOCATION void instrument_data_temp::PrintTo(Print &stream)
    {
        stream.print("Sample Count: "); stream.print(sample_count);
        stream.print("\n");
        for (int i = 0;i<sample_count;i++)
        {
            stream.print("Sample: "); stream.print(i);
            stream.print(", Range max:"); stream.print(sample_note_ranges[i]);
            stream.print("\n");
            samples[i].PrintTo(stream);
        }
    }

    CODE_LOCATION void sfVersionTag::PrintTo(Print &stream)
    {
        stream.print(major);
        stream.print(".");
        if (minor < 10) stream.print("0");
        stream.print(minor);
    }

    CODE_LOCATION void INFO::PrintTo(Print &stream)
    {
        stream.println();
        stream.print("*** Info *** ( size: ");
        stream.print(size);
        stream.println(" )\n");
        stream.print("Soundfont version: "); ifil.PrintTo(stream);
        stream.print("Name: "); stream.println(INAM);
        stream.print("SoundEngine: "); stream.println(isng);
        stream.print("ROM: "); stream.println(irom);
        stream.print("ROM ver: "); iver.PrintTo(stream);
        stream.print("Date: "); stream.println(ICRD);
        stream.print("Credits: "); stream.println(IENG);
        stream.print("Product: "); stream.println(IPRD);
        stream.print("Copyright: "); stream.println(ICOP);
        stream.print("Comment: "); stream.println(ICMT);
        stream.print("Tools: "); stream.println(ISFT);
    }

    CODE_LOCATION pdta_rec::pdta_rec()
    {
        phdr = new phdr_rec[0];
        pbag = new bag_rec[0];
        pmod = new mod_rec[0];
        pgen = new gen_rec[0];
        inst = new inst_rec[0];
        ibag = new bag_rec[0];
        imod = new mod_rec[0];
        igen = new gen_rec[0];
        shdr = new shdr_rec[0];
    }

    CODE_LOCATION void pdta_rec_lazy::CloneInto(pdta_rec_lazy &other)
    {
        other.size = size;
        other.ibag_count = ibag_count;
        other.ibag_position = ibag_position;
        other.igen_count = igen_count;
        other.igen_position = igen_position;
        other.imod_count = imod_count;
        other.imod_position = imod_position;
        other.inst_count = inst_count;
        other.inst_position = inst_position;
        other.pbag_count = pbag_count;
        other.pbag_position = pbag_position;
        other.pgen_count =  pgen_count;
        other.pgen_position = pgen_position;
        other.phdr_count = phdr_count;
        other.phdr_position = phdr_position;
        other.pmod_count = pmod_count;
        other.pmod_position = pmod_position;
        other.shdr_count = shdr_count;
        other.shdr_position = shdr_position;
    }

    CODE_LOCATION void smpl_rec::CloneInto(smpl_rec &other)
    {
        other.size = size;
        other.position = position;
    }

    CODE_LOCATION void sdta_rec_lazy::CloneInto(sdta_rec_lazy &other)
    {
        other.size = size;
        smpl.CloneInto(other.smpl);
        sm24.CloneInto(other.sm24);
    }

    CODE_LOCATION void sfbk_rec_lazy::CloneInto(sfbk_rec_lazy &other)
    {
        other.size = size;
        other.info_position = info_position;
        other.info_size = info_size;
        sdta.CloneInto(other.sdta);
        pdta.CloneInto(other.pdta);
    }
}