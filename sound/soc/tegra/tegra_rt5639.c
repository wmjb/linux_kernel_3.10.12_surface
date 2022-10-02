/*
 * tegra_rt5639.c - Tegra machine ASoC driver for boards using ALC5639 codec.
 *
 * Copyright (c) 2013, NVIDIA CORPORATION. All rights reserved.
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <asm/mach-types.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/edp.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif
#include <linux/pm_runtime.h>
#include <mach/tegra_asoc_pdata.h>
#include <mach/gpio-tegra.h>
#include <mach/tegra_rt5640_pdata.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "../codecs/rt5639.h"

#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"
#include <linux/tfa9887.h>
#include "tegra30_ahub.h"
#include "tegra30_i2s.h"

#define DRV_NAME "tegra-snd-rt5639"

#define DAI_LINK_HIFI		0
#define DAI_LINK_SPDIF		1
#define DAI_LINK_BTSCO		2
#define NUM_DAI_LINKS		3

const char *tegra_rt5639_i2s_dai_name[TEGRA30_NR_I2S_IFC] = {
	"tegra30-i2s.0",
	"tegra30-i2s.1",
	"tegra30-i2s.2",
	"tegra30-i2s.3",
	"tegra30-i2s.4",
};

#define GPIO_SPKR_EN    BIT(0)
#define GPIO_HP_MUTE    BIT(1)
#define GPIO_INT_MIC_EN BIT(2)
#define GPIO_EXT_MIC_EN BIT(3)
#define GPIO_HP_DET     BIT(4)

struct tegra_rt5639 {
	struct tegra_asoc_utils_data util_data;
	struct tegra_asoc_platform_data *pdata;
	struct edp_client *spk_edp_client;
	int gpio_requested;
#ifdef CONFIG_SWITCH
	int jack_status;
#endif
	enum snd_soc_bias_level bias_level;
	int clock_enabled;
	struct regulator *codec_reg;
	struct regulator *digital_reg;
	struct regulator *analog_reg;
	struct regulator *spk_reg;
	struct regulator *dmic_reg;
	struct snd_soc_card *pcard;
};

static int tegra_rt5639_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(cpu_dai);

	tegra_asoc_utils_tristate_dap(i2s->id, false);

	return 0;
}

static void tegra_rt5639_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(cpu_dai);

	tegra_asoc_utils_tristate_dap(i2s->id, true);
}

static int tegra_rt5639_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	int srate, mclk, i2s_daifmt, codec_daifmt;
	int err, rate, sample_size;
	unsigned int i2sclock;

	srate = params_rate(params);
	mclk = 256 * srate;

	i2s_daifmt = SND_SOC_DAIFMT_NB_NF;
	i2s_daifmt |= pdata->i2s_param[HIFI_CODEC].is_i2s_master ?
			SND_SOC_DAIFMT_CBS_CFS : SND_SOC_DAIFMT_CBM_CFM;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S8:
		sample_size = 8;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
		sample_size = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		sample_size = 24;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		sample_size = 32;
		break;
	default:
		return -EINVAL;
	}

	switch (pdata->i2s_param[HIFI_CODEC].i2s_mode) {
	case TEGRA_DAIFMT_I2S:
		i2s_daifmt |= SND_SOC_DAIFMT_I2S;
		break;
	case TEGRA_DAIFMT_DSP_A:
		i2s_daifmt |= SND_SOC_DAIFMT_DSP_A;
		break;
	case TEGRA_DAIFMT_DSP_B:
		i2s_daifmt |= SND_SOC_DAIFMT_DSP_B;
		break;
	case TEGRA_DAIFMT_LEFT_J:
		i2s_daifmt |= SND_SOC_DAIFMT_LEFT_J;
		break;
	case TEGRA_DAIFMT_RIGHT_J:
		i2s_daifmt |= SND_SOC_DAIFMT_RIGHT_J;
		break;
	default:
		dev_err(card->dev, "Can't configure i2s format\n");
		return -EINVAL;
	}

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % mclk)) {
			mclk = machine->util_data.set_mclk;
		} else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	rate = clk_get_rate(machine->util_data.clk_cdev1);

	if (pdata->i2s_param[HIFI_CODEC].is_i2s_master) {
		err = snd_soc_dai_set_sysclk(codec_dai, RT5639_SCLK_S_MCLK,
				rate, SND_SOC_CLOCK_IN);
		if (err < 0) {
			dev_err(card->dev, "codec_dai clock not set\n");
			return err;
		}
	} else {
		err = snd_soc_dai_set_pll(codec_dai, 0, RT5639_PLL1_S_MCLK,
				rate, 512*srate);
		if (err < 0) {
			dev_err(card->dev, "codec_dai pll not set\n");
			return err;
		}
		err = snd_soc_dai_set_sysclk(codec_dai, RT5639_SCLK_S_PLL1,
				512*srate, SND_SOC_CLOCK_IN);
		if (err < 0) {
			dev_err(card->dev, "codec_dai clock not set\n");
			return err;
		}
	}

	/*for 24 bit audio we support only S24_LE (S24_3LE is not supported)
	which is rendered on bus in 32 bits packet so consider as 32 bit
	depth in clock calculations, extra 4 is required by codec,
	God knows why ?*/
	if (sample_size == 24)
		i2sclock = srate * params_channels(params) * 32 * 4;
	else
		i2sclock = 0;

	err = snd_soc_dai_set_sysclk(cpu_dai, 0,
			i2sclock, SND_SOC_CLOCK_OUT);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai clock not set\n");
		return err;
	}

	codec_daifmt = i2s_daifmt;

	/*invert the codec bclk polarity when codec is master
	in DSP mode this is done to match with the negative
	edge settings of tegra i2s*/
	if (((i2s_daifmt & SND_SOC_DAIFMT_FORMAT_MASK)
		== SND_SOC_DAIFMT_DSP_A) &&
		((i2s_daifmt & SND_SOC_DAIFMT_MASTER_MASK)
		== SND_SOC_DAIFMT_CBM_CFM)) {
		codec_daifmt &= ~(SND_SOC_DAIFMT_INV_MASK);
		codec_daifmt |= SND_SOC_DAIFMT_IB_NF;
	}

	err = snd_soc_dai_set_fmt(codec_dai, codec_daifmt);
	if (err < 0) {
		dev_err(card->dev, "codec_dai fmt not set\n");
		return err;
	}

	err = snd_soc_dai_set_fmt(cpu_dai, i2s_daifmt);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai fmt not set\n");
		return err;
	}

	return 0;
}

static int tegra_bt_sco_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	int srate, mclk, min_mclk, i2s_daifmt;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		mclk = 11289600;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
		mclk = 12288000;
		break;
	default:
		return -EINVAL;
	}
	min_mclk = 64 * srate;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % min_mclk))
			mclk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	i2s_daifmt = SND_SOC_DAIFMT_NB_NF;
	i2s_daifmt |= pdata->i2s_param[BT_SCO].is_i2s_master ?
			SND_SOC_DAIFMT_CBS_CFS : SND_SOC_DAIFMT_CBM_CFM;

	switch (pdata->i2s_param[BT_SCO].i2s_mode) {
	case TEGRA_DAIFMT_I2S:
		i2s_daifmt |= SND_SOC_DAIFMT_I2S;
		break;
	case TEGRA_DAIFMT_DSP_A:
		i2s_daifmt |= SND_SOC_DAIFMT_DSP_A;
		break;
	case TEGRA_DAIFMT_DSP_B:
		i2s_daifmt |= SND_SOC_DAIFMT_DSP_B;
		break;
	case TEGRA_DAIFMT_LEFT_J:
		i2s_daifmt |= SND_SOC_DAIFMT_LEFT_J;
		break;
	case TEGRA_DAIFMT_RIGHT_J:
		i2s_daifmt |= SND_SOC_DAIFMT_RIGHT_J;
		break;
	default:
		dev_err(card->dev, "Can't configure i2s format\n");
		return -EINVAL;
	}

	err = snd_soc_dai_set_fmt(rtd->cpu_dai, i2s_daifmt);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai fmt not set\n");
		return err;
	}

	return 0;
}

static int tegra_spdif_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	int srate, mclk, min_mclk;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		mclk = 11289600;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
		mclk = 12288000;
		break;
	default:
		return -EINVAL;
	}
	min_mclk = 128 * srate;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % min_mclk))
			mclk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	return 0;
}

static int tegra_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(rtd->card);

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 0);

	return 0;
}

static struct snd_soc_ops tegra_rt5639_ops = {
	.hw_params = tegra_rt5639_hw_params,
	.hw_free = tegra_hw_free,
	.startup = tegra_rt5639_startup,
	.shutdown = tegra_rt5639_shutdown,
};

static struct snd_soc_ops tegra_rt5639_bt_sco_ops = {
	.hw_params = tegra_bt_sco_hw_params,
	.hw_free = tegra_hw_free,
	.startup = tegra_rt5639_startup,
	.shutdown = tegra_rt5639_shutdown,
};

static struct snd_soc_ops tegra_spdif_ops = {
	.hw_params = tegra_spdif_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_jack tegra_rt5639_hp_jack;

static struct snd_soc_jack_gpio tegra_rt5639_hp_jack_gpio = {
	.name = "headphone detect",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 150,
};

#ifdef CONFIG_SWITCH
/* These values are copied from Android WiredAccessoryObserver */
enum headset_state {
	BIT_NO_HEADSET = 0,
	BIT_HEADSET = (1 << 0),
	BIT_HEADSET_NO_MIC = (1 << 1),
};

static struct switch_dev tegra_rt5639_headset_switch = {
	.name = "h2w",
};

static int tegra_rt5639_jack_notifier(struct notifier_block *self,
			      unsigned long action, void *dev)
{
	struct snd_soc_jack *jack = dev;
	struct snd_soc_codec *codec = jack->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	enum headset_state state = BIT_NO_HEADSET;
	unsigned char status_jack = 0;

	if (jack == &tegra_rt5639_hp_jack) {
		if (action) {
			/* Enable ext mic; enable signal is active-low */
			if (gpio_is_valid(pdata->gpio_ext_mic_en))
				gpio_direction_output(
				pdata->gpio_ext_mic_en, 0);

			status_jack = rt5639_headset_detect(codec, 1);

			machine->jack_status &= ~SND_JACK_HEADPHONE;
			machine->jack_status &= ~SND_JACK_MICROPHONE;

			if (status_jack == RT5639_HEADPHO_DET)
				machine->jack_status |=
				SND_JACK_HEADPHONE;
			else if (status_jack == RT5639_HEADSET_DET) {
					machine->jack_status |=
							SND_JACK_HEADPHONE;
					machine->jack_status |=
							SND_JACK_MICROPHONE;
			}
		} else {
			/* Disable ext mic; enable signal is active-low */
			if (gpio_is_valid(pdata->gpio_ext_mic_en))
				gpio_direction_output(
				pdata->gpio_ext_mic_en, 1);

			rt5639_headset_detect(codec, 0);

			machine->jack_status &= ~SND_JACK_HEADPHONE;
			machine->jack_status &= ~SND_JACK_MICROPHONE;
		}
	}

	switch (machine->jack_status) {
	case SND_JACK_HEADPHONE:
		state = BIT_HEADSET_NO_MIC;
		break;
	case SND_JACK_HEADSET:
		state = BIT_HEADSET;
		break;
	case SND_JACK_MICROPHONE:
		/* mic: would not report */
	default:
		state = BIT_NO_HEADSET;
	}

	switch_set_state(&tegra_rt5639_headset_switch, state);

	return NOTIFY_OK;
}

static struct notifier_block tegra_rt5639_jack_detect_nb = {
	.notifier_call = tegra_rt5639_jack_notifier,
};
#else
static struct snd_soc_jack_pin tegra_rt5639_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

#endif

static void tegra_speaker_edp_set_volume(struct snd_soc_codec *codec,
					 int l_vol,
					 int r_vol)
{
	snd_soc_update_bits(codec,
			    RT5639_SPK_VOL,
			    RT5639_L_VOL_MASK,
			    l_vol << RT5639_L_VOL_SFT);
	snd_soc_update_bits(codec,
			    RT5639_SPK_VOL,
			    RT5639_R_VOL_MASK,
			    r_vol << RT5639_R_VOL_SFT);
}

static void tegra_speaker_throttle(unsigned int new_state,  void *priv_data)
{
	struct tegra_rt5639 *machine = priv_data;
	struct snd_soc_card *card;
	struct snd_soc_codec *codec;

	if (!machine)
		return;

	card = machine->pcard;
	codec = card->rtd[DAI_LINK_HIFI].codec;

	/* set codec volume to reflect the new E-state */
	switch (new_state) {
	case TEGRA_SPK_EDP_NEG_1:
		/* set codec voulme to 0 dB (100%), E-1 state */
		tegra_speaker_edp_set_volume(codec, 0x8, 0x8);
		break;
	case TEGRA_SPK_EDP_ZERO:
		/* set codec volume to -16.5dB (78%), E0 state */
		tegra_speaker_edp_set_volume(codec, 0x13, 0x13);
		break;
	default:
		pr_err("%s: New E-state %d don't support!\n",
			__func__, new_state);
		break;
	}

}


static int tegra_rt5639_event_int_spk(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	int ret = 0;
	int err;
	struct snd_soc_codec *codec = card->rtd[DAI_LINK_HIFI].codec;
	unsigned int approved = TEGRA_SPK_EDP_NUM_STATES;


	if (machine->spk_edp_client == NULL) {
		if (machine->spk_reg) {
			if (SND_SOC_DAPM_EVENT_ON(event))
				err = regulator_enable(machine->spk_reg);
			else
				regulator_disable(machine->spk_reg);
		}
		goto err_null_spk_edp_client;
	}

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		ret = edp_update_client_request(machine->spk_edp_client,
						TEGRA_SPK_EDP_NEG_1,
						&approved);
		err = regulator_enable(machine->spk_reg);
		if (ret || approved != TEGRA_SPK_EDP_NEG_1) {
			if (approved == TEGRA_SPK_EDP_ZERO)
				/* set codec volume to -16.5dB (78%),E0 state */
				tegra_speaker_edp_set_volume(codec, 0x13, 0x13);
		} else {
			/* set codec voulme to 0 dB (100%), E-1 state */
			tegra_speaker_edp_set_volume(codec, 0x8, 0x8);
		}
	} else {
		/* turn off codec volume,-46.5 dB, E1 state */
		tegra_speaker_edp_set_volume(codec, 0x27, 0x27);
		regulator_disable(machine->spk_reg);
		ret = edp_update_client_request(machine->spk_edp_client,
						TEGRA_SPK_EDP_1,
						NULL);
		if (ret) {
			dev_err(card->dev,
				"E+1 state transition failed\n");
		}
	}

err_null_spk_edp_client:
	if (!(machine->gpio_requested & GPIO_SPKR_EN))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_spkr_en,
				!!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int tegra_rt5639_event_hp(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;

	if (!(machine->gpio_requested & GPIO_HP_MUTE))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_hp_mute,
				!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int tegra_rt5639_event_int_mic(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
    int ret = 0;

	if (machine->dmic_reg) {
		if (SND_SOC_DAPM_EVENT_ON(event))
			ret = regulator_enable(machine->dmic_reg);
		else
			regulator_disable(machine->dmic_reg);
	}

	if (!(machine->gpio_requested & GPIO_INT_MIC_EN))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_int_mic_en,
				!!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int tegra_rt5639_event_ext_mic(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;

	if (!(machine->gpio_requested & GPIO_EXT_MIC_EN))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_ext_mic_en,
				!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static const struct snd_soc_dapm_widget ardbeg_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Int Spk", tegra_rt5639_event_int_spk),
	SND_SOC_DAPM_HP("Headphone Jack", tegra_rt5639_event_hp),
	SND_SOC_DAPM_MIC("Mic Jack", tegra_rt5639_event_ext_mic),
	SND_SOC_DAPM_MIC("Int Mic", tegra_rt5639_event_int_mic),
};

static const struct snd_soc_dapm_route ardbeg_audio_map[] = {
	{"Headphone Jack", NULL, "HPOR"},
	{"Headphone Jack", NULL, "HPOL"},
	{"Int Spk", NULL, "SPORP"},
	{"Int Spk", NULL, "SPORN"},
	{"Int Spk", NULL, "SPOLP"},
	{"Int Spk", NULL, "SPOLN"},
	{"micbias1", NULL, "Mic Jack"},
	{"IN2P", NULL, "micbias1"},
	{"IN2N", NULL, "micbias1"},
	{"DMIC L1", NULL, "Int Mic"},
	{"DMIC L2", NULL, "Int Mic"},
	{"DMIC R1", NULL, "Int Mic"},
	{"DMIC R2", NULL, "Int Mic"},
	/*{"micbias1", NULL, "Int Mic"},*/
	/*{"IN1P", NULL, "micbias1"},*/
	/*{"IN1N", NULL, "micbias1"},*/
};


static const struct snd_kcontrol_new ardbeg_controls[] = {
	SOC_DAPM_PIN_SWITCH("Int Spk"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Mic Jack"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
};

static int tegra_rt5639_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = codec->card;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	int ret;

	if (gpio_is_valid(pdata->gpio_hp_det)) {
		tegra_rt5639_hp_jack_gpio.gpio = pdata->gpio_hp_det;
		tegra_rt5639_hp_jack_gpio.invert =
			!pdata->gpio_hp_det_active_high;
		snd_soc_jack_new(codec, "Headphone Jack", SND_JACK_HEADPHONE,
				&tegra_rt5639_hp_jack);
#ifndef CONFIG_SWITCH
		snd_soc_jack_add_pins(&tegra_rt5639_hp_jack,
					ARRAY_SIZE(tegra_rt5639_hp_jack_pins),
					tegra_rt5639_hp_jack_pins);
#else
		snd_soc_jack_notifier_register(&tegra_rt5639_hp_jack,
					&tegra_rt5639_jack_detect_nb);
#endif
		snd_soc_jack_add_gpios(&tegra_rt5639_hp_jack,
					1,
					&tegra_rt5639_hp_jack_gpio);
		machine->gpio_requested |= GPIO_HP_DET;
	}

	ret = tegra_asoc_utils_register_ctls(&machine->util_data);
	if (ret < 0)
		return ret;

	/* FIXME: Calculate automatically based on DAPM routes? */
	snd_soc_dapm_nc_pin(dapm, "LOUTL");
	snd_soc_dapm_nc_pin(dapm, "LOUTR");

	snd_soc_dapm_sync(dapm);

	return 0;
}

static struct snd_soc_dai_link tegra_rt5639_dai[NUM_DAI_LINKS] = {
	[DAI_LINK_HIFI] = {
		.name = "rt5639",
		.stream_name = "rt5639 PCM",
		.codec_name = "rt5639.0-001a",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra30-i2s.1",
		.codec_dai_name = "rt5639-aif1",
		.init = tegra_rt5639_init,
		.ops = &tegra_rt5639_ops,
	},

	[DAI_LINK_SPDIF] = {
		.name = "SPDIF",
		.stream_name = "SPDIF PCM",
		.codec_name = "spdif-dit.0",
		.platform_name = "tegra30-spdif",
		.cpu_dai_name = "tegra30-spdif",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_spdif_ops,
	},

	[DAI_LINK_BTSCO] = {
		.name = "BT-SCO",
		.stream_name = "BT SCO PCM",
		.codec_name = "spdif-dit.1",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra30-i2s.3",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_rt5639_bt_sco_ops,
	},
};

static int tegra_rt5639_suspend_post(struct snd_soc_card *card)
{
	struct snd_soc_jack_gpio *gpio = &tegra_rt5639_hp_jack_gpio;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	int i, suspend_allowed = 1;

	/*In Voice Call we ignore suspend..so check for that*/
	for (i = 0; i < machine->pcard->num_links; i++) {
		if (machine->pcard->dai_link[i].ignore_suspend) {
			suspend_allowed = 0;
			break;
		}
	}

	if (suspend_allowed) {
		/*Disable the irq so that device goes to suspend*/
		if (gpio_is_valid(gpio->gpio))
			disable_irq(gpio_to_irq(gpio->gpio));
		/*This may be required if dapm setbias level is not called in
		some cases, may be due to a wrong dapm map*/
		if (machine->clock_enabled) {
			machine->clock_enabled = 0;
			tegra_asoc_utils_clk_disable(&machine->util_data);
		}
		/*TODO: Disable Audio Regulators*/
	}

	return 0;
}

static int tegra_rt5639_resume_pre(struct snd_soc_card *card)
{
	int val;
	struct snd_soc_jack_gpio *gpio = &tegra_rt5639_hp_jack_gpio;
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	int i, suspend_allowed = 1;

	/*In Voice Call we ignore suspend..so check for that*/
	for (i = 0; i < machine->pcard->num_links; i++) {
		if (machine->pcard->dai_link[i].ignore_suspend) {
			suspend_allowed = 0;
			break;
		}
	}

	if (suspend_allowed) {
		/*Convey jack status after resume and
		re-enable the interrupts*/
		if (gpio_is_valid(gpio->gpio)) {
			val = gpio_get_value(gpio->gpio);
			val = gpio->invert ? !val : val;
			snd_soc_jack_report(gpio->jack, val, gpio->report);
			enable_irq(gpio_to_irq(gpio->gpio));
		}
		/*This may be required if dapm setbias level is not called in
		some cases, may be due to a wrong dapm map*/
		if (!machine->clock_enabled &&
				machine->bias_level != SND_SOC_BIAS_OFF) {
			machine->clock_enabled = 1;
			tegra_asoc_utils_clk_enable(&machine->util_data);
		}
		/*TODO: Enable Audio Regulators*/
	}

	return 0;
}

static int tegra_rt5639_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);

	if (machine->bias_level == SND_SOC_BIAS_OFF &&
		level != SND_SOC_BIAS_OFF && (!machine->clock_enabled)) {
		machine->clock_enabled = 1;
		tegra_asoc_utils_clk_enable(&machine->util_data);
		machine->bias_level = level;
	}

	return 0;
}

static int tegra_rt5639_set_bias_level_post(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);

	if (machine->bias_level != SND_SOC_BIAS_OFF &&
		level == SND_SOC_BIAS_OFF && machine->clock_enabled) {
		machine->clock_enabled = 0;
		tegra_asoc_utils_clk_disable(&machine->util_data);
	}

	machine->bias_level = level;

	return 0 ;
}

static struct snd_soc_card snd_soc_tegra_rt5639 = {
	.name = "tegra-rt5639",
	.owner = THIS_MODULE,
	.dai_link = tegra_rt5639_dai,
	.num_links = ARRAY_SIZE(tegra_rt5639_dai),
	.suspend_post = tegra_rt5639_suspend_post,
	.resume_pre = tegra_rt5639_resume_pre,
	.set_bias_level = tegra_rt5639_set_bias_level,
	.set_bias_level_post = tegra_rt5639_set_bias_level_post,
	.controls = ardbeg_controls,
	.num_controls = ARRAY_SIZE(ardbeg_controls),
	.dapm_widgets = ardbeg_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(ardbeg_dapm_widgets),
	.dapm_routes = ardbeg_audio_map,
	.num_dapm_routes = ARRAY_SIZE(ardbeg_audio_map),
	.fully_routed = true,
};

static int tegra_rt5639_driver_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_tegra_rt5639;
	struct device_node *np = pdev->dev.of_node;
	struct tegra_rt5639 *machine;
	struct tegra_asoc_platform_data *pdata = NULL;
	struct snd_soc_codec *codec;
	struct edp_manager *battery_manager = NULL;
	int ret;
	int codec_id;
	u32 val32[7];

	if (!pdev->dev.platform_data && !pdev->dev.of_node) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}
	if (pdev->dev.platform_data) {
		pdata = pdev->dev.platform_data;
	} else if (np) {
		pdata = kzalloc(sizeof(struct tegra_asoc_platform_data),
			GFP_KERNEL);
		if (!pdata) {
			dev_err(&pdev->dev, "Can't allocate tegra_asoc_platform_data struct\n");
			return -ENOMEM;
		}

		of_property_read_string(np, "nvidia,codec_name",
					&pdata->codec_name);

		of_property_read_string(np, "nvidia,codec_dai_name",
					&pdata->codec_dai_name);

		pdata->gpio_ldo1_en = of_get_named_gpio(np,
						"nvidia,ldo-gpios", 0);
		if (pdata->gpio_ldo1_en < 0)
			dev_warn(&pdev->dev, "Failed to get LDO_EN GPIO\n");

		pdata->gpio_hp_det = of_get_named_gpio(np,
						"nvidia,hp-det-gpios", 0);
		if (pdata->gpio_hp_det < 0)
			dev_warn(&pdev->dev, "Failed to get HP Det GPIO\n");

		pdata->gpio_codec1 = pdata->gpio_codec2 = pdata->gpio_codec3 =
		pdata->gpio_spkr_en = pdata->gpio_hp_mute =
		pdata->gpio_int_mic_en = pdata->gpio_ext_mic_en = -1;

		of_property_read_u32_array(np, "nvidia,i2s-param-hifi", val32,
							   ARRAY_SIZE(val32));
		pdata->i2s_param[HIFI_CODEC].audio_port_id = (int)val32[0];
		pdata->i2s_param[HIFI_CODEC].is_i2s_master = (int)val32[1];
		pdata->i2s_param[HIFI_CODEC].i2s_mode = (int)val32[2];

		of_property_read_u32_array(np, "nvidia,i2s-param-bt", val32,
							   ARRAY_SIZE(val32));
		pdata->i2s_param[BT_SCO].audio_port_id = (int)val32[0];
		pdata->i2s_param[BT_SCO].is_i2s_master = (int)val32[1];
		pdata->i2s_param[BT_SCO].i2s_mode = (int)val32[2];
	}

	if (!pdata) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}

	if (pdata->codec_name)
		card->dai_link->codec_name = pdata->codec_name;

	if (pdata->codec_dai_name)
		card->dai_link->codec_dai_name = pdata->codec_dai_name;

	machine = kzalloc(sizeof(struct tegra_rt5639), GFP_KERNEL);
	if (!machine) {
		dev_err(&pdev->dev, "Can't allocate tegra_rt5639 struct\n");
		if (np)
			kfree(pdata);
		return -ENOMEM;
	}

	if (gpio_is_valid(pdata->gpio_ldo1_en)) {
		ret = gpio_request(pdata->gpio_ldo1_en, "rt5639");
		if (ret)
			dev_err(&pdev->dev, "Fail gpio_request AUDIO_LDO1\n");
		else {
			ret = gpio_direction_output(pdata->gpio_ldo1_en, 1);
			if (ret)
				dev_err(&pdev->dev, "Fail gpio_direction AUDIO_LDO1\n");
		}
		msleep(200);
	}

	machine->pdata = pdata;
	machine->pcard = card;
	machine->bias_level = SND_SOC_BIAS_STANDBY;
	machine->clock_enabled = 1;

	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev, card);
	if (ret)
		goto err_free_machine;

	/*
	*codec_reg - its a GPIO (in the form of a fixed regulator) that enables
	*the basic(I2C) power for the codec and must be ON always
	*/
	if (!gpio_is_valid(pdata->gpio_ldo1_en)) {
		machine->codec_reg = regulator_get(&pdev->dev, "ldoen");
		if (IS_ERR(machine->codec_reg))
			machine->codec_reg = 0;
		else
			ret = regulator_enable(machine->codec_reg);
	}

	/*
	*digital_reg - provided the digital power for the codec and must be
	*ON always
	*/
	machine->digital_reg = regulator_get(&pdev->dev, "dbvdd");
	if (IS_ERR(machine->digital_reg))
		machine->digital_reg = 0;
	else
		ret = regulator_enable(machine->digital_reg);

	/*
	*analog_reg - provided the analog power for the codec and must be
	*ON always
	*/
	machine->analog_reg = regulator_get(&pdev->dev, "avdd");
	if (IS_ERR(machine->analog_reg))
		machine->analog_reg = 0;
	else
		ret = regulator_enable(machine->analog_reg);

	/*
	*spk_reg - provided the speaker power and can be turned ON
	*on need basis, when required
	*/
	machine->spk_reg = regulator_get(&pdev->dev, "spkvdd");
	if (IS_ERR(machine->spk_reg))
		machine->spk_reg = 0;

	/*
	*dmic_reg - provided the DMIC power and can be turned ON
	*on need basis, when required
	*/
	machine->dmic_reg = regulator_get(&pdev->dev, "dmicvdd");
	if (IS_ERR(machine->dmic_reg))
		machine->dmic_reg = 0;


#ifdef CONFIG_SWITCH
	/* Addd h2w swith class support */
	ret = tegra_asoc_switch_register(&tegra_rt5639_headset_switch);
	if (ret < 0)
		goto err_fini_utils;
#endif

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, machine);

#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	codec_id = pdata->i2s_param[HIFI_CODEC].audio_port_id;
	tegra_rt5639_dai[DAI_LINK_HIFI].cpu_dai_name =
	tegra_rt5639_i2s_dai_name[codec_id];
	tegra_rt5639_dai[DAI_LINK_HIFI].platform_name =
	tegra_rt5639_i2s_dai_name[codec_id];

	codec_id = pdata->i2s_param[BT_SCO].audio_port_id;
	tegra_rt5639_dai[DAI_LINK_BTSCO].cpu_dai_name =
	tegra_rt5639_i2s_dai_name[codec_id];
	tegra_rt5639_dai[DAI_LINK_BTSCO].platform_name =
	tegra_rt5639_i2s_dai_name[codec_id];
#endif

	card->dapm.idle_bias_off = 1;
	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err_unregister_switch;
	}

	if (!card->instantiated) {
		ret = -ENODEV;
		dev_err(&pdev->dev, "sound card not instantiated (%d)\n",
			ret);
		goto err_unregister_card;
	}

#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	ret = tegra_asoc_utils_set_parent(&machine->util_data,
				pdata->i2s_param[HIFI_CODEC].is_i2s_master);
	if (ret) {
		dev_err(&pdev->dev, "tegra_asoc_utils_set_parent failed (%d)\n",
			ret);
		goto err_unregister_card;
	}
#endif


	if (!pdata->edp_support)
		return 0;

	machine->spk_edp_client = devm_kzalloc(&pdev->dev,
				sizeof(struct edp_client), GFP_KERNEL);
	if (IS_ERR_OR_NULL(machine->spk_edp_client)) {
		dev_err(&pdev->dev, "could not allocate edp client\n");
		return 0;
	}

	strncpy(machine->spk_edp_client->name, "speaker", EDP_NAME_LEN - 1);
	machine->spk_edp_client->name[EDP_NAME_LEN - 1] = '\0';
	machine->spk_edp_client->states = pdata->edp_states;
	machine->spk_edp_client->num_states = TEGRA_SPK_EDP_NUM_STATES;
	machine->spk_edp_client->e0_index = TEGRA_SPK_EDP_ZERO;
	machine->spk_edp_client->priority = EDP_MAX_PRIO + 2;
	machine->spk_edp_client->throttle = tegra_speaker_throttle;
	machine->spk_edp_client->private_data = machine;

	battery_manager = edp_get_manager("battery");
	if (!battery_manager) {
		dev_err(&pdev->dev, "unable to get edp manager\n");
	} else {
		/* register speaker edp client */
		ret = edp_register_client(battery_manager,
			machine->spk_edp_client);
		if (ret) {
			dev_err(&pdev->dev, "unable to register edp client\n");
				devm_kfree(&pdev->dev, machine->spk_edp_client);
				machine->spk_edp_client = NULL;
				return 0;
		}
		codec = card->rtd[DAI_LINK_HIFI].codec;

		/* Default turn off codec, set E1 state */
		tegra_speaker_edp_set_volume(codec, 0x27, 0x27);
		ret = edp_update_client_request(machine->spk_edp_client,
						TEGRA_SPK_EDP_1,
						NULL);
		if (ret) {
			dev_err(&pdev->dev,
				"unable to set E1 EDP state\n");
			edp_unregister_client(machine->spk_edp_client);
			devm_kfree(&pdev->dev, machine->spk_edp_client);
			machine->spk_edp_client = NULL;
			return 0;
		}
	}

	return 0;

err_unregister_card:
	snd_soc_unregister_card(card);
err_unregister_switch:
#ifdef CONFIG_SWITCH
	tegra_asoc_switch_unregister(&tegra_rt5639_headset_switch);
err_fini_utils:
#endif
	tegra_asoc_utils_fini(&machine->util_data);
err_free_machine:
	if (np)
		kfree(machine->pdata);

	kfree(machine);

	return ret;
}

static int tegra_rt5639_driver_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_rt5639 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	struct device_node *np = pdev->dev.of_node;

	if (machine->gpio_requested & GPIO_HP_DET)
		snd_soc_jack_free_gpios(&tegra_rt5639_hp_jack,
					1,
					&tegra_rt5639_hp_jack_gpio);

	if (machine->digital_reg)
		regulator_put(machine->digital_reg);
	if (machine->analog_reg)
		regulator_put(machine->analog_reg);
	if (machine->spk_reg)
		regulator_put(machine->spk_reg);
	if (machine->dmic_reg)
		regulator_put(machine->dmic_reg);
	if (machine->codec_reg)
		regulator_put(machine->codec_reg);

	if (gpio_is_valid(pdata->gpio_ldo1_en)) {
		gpio_set_value(pdata->gpio_ldo1_en, 0);
		gpio_free(pdata->gpio_ldo1_en);
	}

	snd_soc_unregister_card(card);

	tegra_asoc_utils_fini(&machine->util_data);

#ifdef CONFIG_SWITCH
	tegra_asoc_switch_unregister(&tegra_rt5639_headset_switch);
#endif
	if (np)
		kfree(machine->pdata);

	kfree(machine);

	return 0;
}

static const struct of_device_id tegra_rt5639_of_match[] = {
	{ .compatible = "nvidia,tegra-audio-rt5639", },
	{},
};

static struct platform_driver tegra_rt5639_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = tegra_rt5639_of_match,
	},
	.probe = tegra_rt5639_driver_probe,
	.remove = tegra_rt5639_driver_remove,
};

static int __init tegra_rt5639_modinit(void)
{
	return platform_driver_register(&tegra_rt5639_driver);
}
module_init(tegra_rt5639_modinit);

static void __exit tegra_rt5639_modexit(void)
{
	platform_driver_unregister(&tegra_rt5639_driver);
}
module_exit(tegra_rt5639_modexit);

MODULE_AUTHOR("Nikesh Oswal <noswal@nvidia.com>");
MODULE_DESCRIPTION("Tegra+rt5639 machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
