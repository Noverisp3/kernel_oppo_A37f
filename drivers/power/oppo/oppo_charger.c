/*******************************************************************************
* Copyright (c)  2014- 2014  Guangdong OPPO Mobile Telecommunications Corp., Ltd
* CONFIG_MACH_OPPO
* Description: Source file for CBufferList.
*           To allocate and free memory block safely.
* Version   : 0.0
* Date      : 2014-07-30
* Author    : Lijiada @Bsp.charge
* ---------------------------------- Revision History: -------------------------
* <version>           <date>          < author >              <desc>
* Revision 0.0        2014-07-30      Lijiada @Bsp.charge
* Modified to be suitable to the new coding rules in all functions.
*******************************************************************************/

#define OPPO_CHARGER_PAR
#include "oppo_inc.h"

#include <linux/kobject.h>
#include <linux/sysfs.h>

bool is_gt1x_tp_charger =false;

extern struct opchg_charger *opchg_chip; /* Battery charger chip */

int force_fast_charge = 1; /* 0 = disabled, 1 = enabled (default) */
static int thermal_disabled = 0; /* 1 = disabled due to high temperature */
static int thermal_protection_enabled = 1; /* 0 = disabled, 1 = enabled (default) */

#define USB_FAST_CHARGE_DANGEROUS_TEMP 450  /* 45°C */
#define USB_FAST_CHARGE_SAFE_TEMP 400       /* 40°C */

static ssize_t force_fast_charge_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", force_fast_charge);
}

static ssize_t thermal_protection_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", thermal_protection_enabled);
}

static ssize_t thermal_protection_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	if (val < 0 || val > 1)
		return -EINVAL;

	thermal_protection_enabled = val;
	return count;
}

static ssize_t force_fast_charge_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	force_fast_charge = val ? 1 : 0;
	return count;
}

static struct kobj_attribute force_fast_charge_attr = __ATTR(force_fast_charge, 0644, force_fast_charge_show, force_fast_charge_store);
static struct kobj_attribute thermal_protection_attr = __ATTR(thermal_protection, 0644, thermal_protection_show, thermal_protection_store);

static struct kobject *fast_charge_kobj;

static int __init oppo_charger_sysfs_init(void)
{
	int retval;

	fast_charge_kobj = kobject_create_and_add("fast_charge", kernel_kobj);
	if (!fast_charge_kobj)
		return -ENOMEM;

	retval = sysfs_create_file(fast_charge_kobj, &force_fast_charge_attr.attr);
	if (retval)
		goto err;

	retval = sysfs_create_file(fast_charge_kobj, &thermal_protection_attr.attr);
	if (retval)
		goto err;

	return retval;

err:
	kobject_put(fast_charge_kobj);
	return retval;
}

static void __exit oppo_charger_sysfs_exit(void)
{
	if (fast_charge_kobj) {
		sysfs_remove_file(fast_charge_kobj, &force_fast_charge_attr.attr);
		sysfs_remove_file(fast_charge_kobj, &thermal_protection_attr.attr);
		kobject_put(fast_charge_kobj);
	}
}

subsys_initcall(oppo_charger_sysfs_init);
module_exit(oppo_charger_sysfs_exit);

static int __opchg_read_reg(struct opchg_charger *chip, u8 reg, u8 *val)
{
	s32 ret=0;

	ret = i2c_smbus_read_byte_data(chip->client, reg);
	if (ret < 0) {
		dev_err(chip->dev, "i2c read fail: can't read from %02x: %d\n", reg, ret);
		return ret;
	} else {
		*val = ret;
	}

	return 0;
}

static int __opchg_write_reg(struct opchg_charger *chip, int reg, u8 val)
{
	s32 ret=0;

	ret = i2c_smbus_write_byte_data(chip->client, reg, val);
	if (ret < 0) {
		dev_err(chip->dev, "i2c write fail: can't write %02x to %02x: %d\n", val, reg, ret);
		return ret;
	}
	return 0;
}

int opchg_read_reg(struct opchg_charger *chip, int reg, u8 *val)
{
	int rc;

	mutex_lock(&chip->read_write_lock);
	rc = __opchg_read_reg(chip, reg, val);
	mutex_unlock(&chip->read_write_lock);

	return rc;
}

int opchg_write_reg(struct opchg_charger *chip, int reg, u8 val)
{
	int rc;

	mutex_lock(&chip->read_write_lock);
	rc = __opchg_write_reg(chip, reg, val);
	mutex_unlock(&chip->read_write_lock);

	return rc;
}

int opchg_masked_write(struct opchg_charger *chip, int reg,
							u8 mask, u8 val)
{
	s32 rc;
	u8 temp;

	mutex_lock(&chip->read_write_lock);
	rc = __opchg_read_reg(chip, reg, &temp);
	if (rc) {
		dev_err(chip->dev, "opchg_read_reg Failed: reg=%03X, rc=%d\n", reg, rc);
		goto out;
	}
	temp &= ~mask;
	temp |= val & mask;
	if(chip->driver_id == OPCHG_BQ24188_ID){
		if((reg == BQ24188_CTRL_REG) && (mask != BQ24188_RESET_MASK)){
			temp &= 0x7F;
		}
	}
	rc = __opchg_write_reg(chip, reg, temp);
	if (rc) {
		dev_err(chip->dev, "opchg_write Failed: reg=%03X, rc=%d\n", reg, rc);
	}
out:
	mutex_unlock(&chip->read_write_lock);
	return rc;
}

int opchg_check_i2c_status(struct opchg_charger *chip)
{
    int rc=0;
	u8 reg = 0;

	if(opchg_chip == NULL)
	{
		return rc;
	}

	switch (opchg_chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = opchg_read_reg(chip, INPUT_CURRENT_LIMIT_REG, &reg);
        break;

	case OPCHG_SMB1357_ID:
		rc=0;
		//rc = opchg_read_reg(chip, REG00_SMB1357_ADDRESS, &reg);
		break;

	case OPCHG_BQ24196_ID:
		rc = opchg_read_reg(chip, REG00_BQ24196_ADDRESS, &reg);
		break;

    default:
        break;
    }

    return rc;
}

#if 1
int opchg_set_otg_enable(void)
{
    int rc=0;

	if(opchg_chip == NULL)
	{
		return rc;
	}

    if (opchg_chip->suspending) {
		return rc;
	}

	switch (opchg_chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = smb358_set_otg_enable();
        break;

	case OPCHG_SMB1357_ID:
		rc=0;
		//rc = smb1357_set_otg_regulator_enable(rdev);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_set_otg_enable();
		break;

	case OPCHG_BQ24157_ID:
		rc = bq24157_set_otg_enable();
		break;

	case OPCHG_BQ24188_ID:
		rc = bq24188_set_otg_enable();
		break;

    default:
        break;
    }

    return rc;
}

int opchg_set_otg_disable(void)
{
    int rc = 0;

    if(opchg_chip == NULL)
	{
		return rc;
	}

	if (opchg_chip->suspending) {
		return rc;
	}

	switch (opchg_chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = smb358_set_otg_disable();
        break;

	case OPCHG_SMB1357_ID:
		rc=0;
		//rc = smb1357_set_otg_regulator_disable(rdev);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_set_otg_disable();
		break;

	case OPCHG_BQ24157_ID:
		rc = bq24157_set_otg_disable();
		break;

	case OPCHG_BQ24188_ID:
		rc = bq24188_set_otg_disable();
		break;

    default:
        break;
    }

    return rc;
}

int opchg_get_otg_enable(void)
{
    int rc = 0;

	if(opchg_chip == NULL)
	{
		return rc;
	}

	if (opchg_chip->suspending) {
		return rc;
	}

	switch (opchg_chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = smb358_get_otg_enable();
        break;

	case OPCHG_SMB1357_ID:
		rc=0;
		//rc = smb1357_get_otg_enable();
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_get_otg_enable();
		break;

	case OPCHG_BQ24157_ID:
		rc = bq24157_get_otg_enable();
		break;

	case OPCHG_BQ24188_ID:
		rc = bq24188_get_otg_enable();
		break;

    default:
        break;
    }

    return rc;
}
#endif

int opchg_set_otg_regulator_enable(struct regulator_dev *rdev)
{
    int rc=0;

    if (opchg_chip->suspending) {
		return rc;
	}

	switch (opchg_chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = smb358_set_otg_regulator_enable(rdev);
        break;

	case OPCHG_SMB1357_ID:
		rc=0;
		//rc = smb1357_set_otg_regulator_enable(rdev);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_set_otg_regulator_enable(rdev);
		break;
	case OPCHG_BQ24157_ID:
		rc = bq24157_set_otg_regulator_enable(rdev);
		break;

	case OPCHG_BQ24188_ID:
		rc = bq24188_set_otg_regulator_enable(rdev);
		break;

    default:
        break;
    }

    return rc;
}

int opchg_set_otg_regulator_disable(struct regulator_dev *rdev)
{
    int rc=0;

    if (opchg_chip->suspending) {
		return rc;
	}

	switch (opchg_chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = smb358_set_otg_regulator_disable(rdev);
        break;

	case OPCHG_SMB1357_ID:
		rc=0;
		//rc = smb1357_set_otg_regulator_disable(rdev);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_set_otg_regulator_disable(rdev);
		break;

	case OPCHG_BQ24157_ID:
		rc = bq24157_set_otg_regulator_disable(rdev);
		break;

	case OPCHG_BQ24188_ID:
		rc = bq24188_set_otg_regulator_disable(rdev);
		break;

    default:
        break;
    }

    return rc;
}

int opchg_get_otg_regulator_is_enable(struct regulator_dev *rdev)
{
    int rc=0;

    if (opchg_chip->suspending) {
		return rc;
	}

	switch (opchg_chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = smb358_get_otg_regulator_is_enable(rdev);
        break;

	case OPCHG_SMB1357_ID:
		rc=0;
		//rc = smb1357_get_otg_regulator_is_enable(rdev);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_get_otg_regulator_is_enable(rdev);
		break;

	case OPCHG_BQ24157_ID:
		rc = bq24157_get_otg_regulator_is_enable(rdev);
		break;

	case OPCHG_BQ24188_ID:
		rc = bq24188_get_otg_regulator_is_enable(rdev);
		break;

    default:
        break;
    }

    return rc;
}

struct regulator_ops opchg_chg_otg_reg_ops = {
    .enable     = opchg_set_otg_regulator_enable,
    .disable    = opchg_set_otg_regulator_disable,
    .is_enabled = opchg_get_otg_regulator_is_enable,
};

int opchg_regulator_init(struct opchg_charger *chip)
{
	int rc = 0;
	struct regulator_init_data *init_data;
	struct regulator_config cfg = {};

	//lfc initial cfg to solve null pointer when gpio_request(cfg.ena_gpio)
	memset(&cfg,0,sizeof(struct regulator_config));
	init_data = of_get_regulator_init_data(chip->dev, chip->dev->of_node);
	if (!init_data) {
		dev_err(chip->dev, "Unable to allocate memory\n");
		return -ENOMEM;
	}

	/* Give the name, then will register */
	if (init_data->constraints.name) {
		chip->otg_vreg.rdesc.owner = THIS_MODULE;
		chip->otg_vreg.rdesc.type = REGULATOR_VOLTAGE;
		chip->otg_vreg.rdesc.ops = &opchg_chg_otg_reg_ops;
		chip->otg_vreg.rdesc.name = init_data->constraints.name;

		cfg.dev = chip->dev;
		cfg.init_data = init_data;
		cfg.driver_data = chip;
		cfg.of_node = chip->dev->of_node;

		init_data->constraints.valid_ops_mask |= REGULATOR_CHANGE_STATUS;

		#if 1
		chip->otg_vreg.rdev = regulator_register(&chip->otg_vreg.rdesc, &cfg);
		dev_err(chip->dev, "oppo_debug OTG set vbus \n");
		#else
		chip->otg_vreg.rdev = regulator_register(
						    &chip->otg_vreg.rdesc, cfg.dev,
						    cfg.init_data, cfg.driver_data,
						    cfg.of_node);
		#endif

		if (IS_ERR(chip->otg_vreg.rdev)) {
			rc = PTR_ERR(chip->otg_vreg.rdev);
			chip->otg_vreg.rdev = NULL;
			if (rc != -EPROBE_DEFER)
				dev_err(chip->dev, "OTG reg failed, rc=%d\n", rc);
		}
	}

	return rc;
}

void opchg_get_prop_fastcharger_type(struct opchg_charger *chip)
{
	int rc = 0;

    if (chip->suspending) {
		return;
	}

	switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        //
        break;

	case OPCHG_SMB1357_ID:
		rc = smb1357_get_prop_fastcharger_type(chip);
		break;

	case OPCHG_BQ24196_ID:
		// BQ24196 Fast Charge: Force higher current limits
		if (force_fast_charge && (!thermal_protection_enabled || !thermal_disabled)) {
			dev_info(chip->dev, "USB Fast Charge: BQ24196 forcing higher current limits\n");
			// Force input current to 1.5A and fast charge current to max
			opchg_set_input_chg_current(chip, 1500, false);
			opchg_set_fast_chg_current(chip, chip->fastchg_current_max_ma);
		}
		break;

    default:
        break;
    }

    /* USB Fast Charge: Enable fast charger type for DCP when forced and thermal safe */
    if (!rc && force_fast_charge && (!thermal_protection_enabled || !thermal_disabled) &&
        qpnp_charger_type_get(chip) == POWER_SUPPLY_TYPE_USB_DCP) {
        rc = true;
    } else if (!rc) {
    }

    chip->fastcharger_type = rc;

    dev_info(chip->dev, "USB Fast Charge: fastcharger_type SET to %d (was %d)\n", rc, chip->fastcharger_type);
}

void opchg_get_prop_charge_type(struct opchg_charger *chip)
{
    int rc;

    if (chip->suspending) {
		return;
	}

	switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = smb358_get_prop_charge_type(chip);
        break;

	case OPCHG_SMB1357_ID:
		rc = smb1357_get_prop_charge_type(chip);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_get_prop_charge_type(chip);
		break;

	case OPCHG_BQ24157_ID:
		rc = bq24157_get_prop_charge_type(chip);
		break;

	case OPCHG_BQ24188_ID:
		rc = bq24188_get_prop_charge_type(chip);
		break;

    default:
        break;
    }

    chip->bat_charging_state = rc;
}

void opchg_get_charging_status(struct opchg_charger *chip)
{
    int rc;

    if (chip->suspending) {
		return;
	}

	switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = smb358_get_charging_status(chip);
        break;

	case OPCHG_SMB1357_ID:
		rc = smb1357_get_charging_status(chip);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_get_charging_status(chip);
		break;

	case OPCHG_BQ24157_ID:
		rc = bq24157_get_charging_status(chip);
		break;

	case OPCHG_BQ24188_ID:
		rc = bq24188_get_charging_status(chip);
		break;

    default:
        break;
    }

    chip->is_charging = rc;
}

int opchg_get_prop_batt_status(struct opchg_charger *chip)
{
    if (chip->suspending)
		return POWER_SUPPLY_STATUS_UNKNOWN;

	if(chip->batt_full){
		if((chip->batt_authen) &&
			((chip->charging_opchg_temp_statu >= OPCHG_CHG_TEMP_PRE_COOL1)&&(chip->charging_opchg_temp_statu  <= OPCHG_CHG_TEMP_NORMAL)))
		{
			if(chip->bat_volt_check_point >= 100)
				chip->bat_status = POWER_SUPPLY_STATUS_FULL;
			else
				chip->bat_status = POWER_SUPPLY_STATUS_CHARGING;
		} else {
			chip->bat_status = POWER_SUPPLY_STATUS_FULL;
		}
		return chip->bat_status;
	} else if(chip->batt_pre_full || chip->batt_ovp){
        chip->bat_status = POWER_SUPPLY_STATUS_CHARGING;
        return chip->bat_status;
	}

    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        chip->bat_status = smb358_get_prop_batt_status(chip);
        break;

	case OPCHG_SMB1357_ID:
		chip->bat_status = smb1357_get_prop_batt_status(chip);
		break;

	case OPCHG_BQ24196_ID:
		chip->bat_status = bq24196_get_prop_batt_status(chip);
		break;

	case OPCHG_BQ24157_ID:
		chip->bat_status = bq24157_get_prop_batt_status(chip);
		break;

	case OPCHG_BQ24188_ID:
		chip->bat_status = bq24188_get_prop_batt_status(chip);
		break;

    default:
        break;
    }

#ifdef OPPO_USE_FAST_CHARGER
	if(is_project(OPPO_14005) || is_project(OPPO_15011) || is_project(OPPO_15018) || is_project(OPPO_15022))
	{
		if(opchg_get_prop_fast_chg_started(chip) == true)
			chip->bat_status = POWER_SUPPLY_STATUS_CHARGING;
	}
#endif

	return chip->bat_status;
}

void opchg_set_enable_volatile_writes(struct opchg_charger *chip)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        smb358_set_enable_volatile_writes(chip);
        break;

	case OPCHG_SMB1357_ID:
		smb1357_set_enable_volatile_writes(chip);
		break;

	case OPCHG_BQ24196_ID:
		bq24196_set_enable_volatile_writes(chip);
		break;

	case OPCHG_BQ24157_ID:
		//
		break;

	case OPCHG_BQ24188_ID:
		bq24188_set_enable_volatile_writes(chip);
		break;

    default:
        break;
    }
}

void opchg_set_complete_charge_timeout(struct opchg_charger *chip, int val)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        smb358_set_complete_charge_timeout(chip, val);
        break;

	case OPCHG_SMB1357_ID:
		smb1357_set_complete_charge_timeout(chip, val);
		break;

	case OPCHG_BQ24196_ID:
		bq24196_set_complete_charge_timeout(chip, val);
		break;
	case OPCHG_BQ24188_ID:
		bq24188_set_complete_charge_timeout(chip, val);
		break;

    default:
        break;
    }
}

void opchg_set_reset_charger(struct opchg_charger *chip, bool reset)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        //smb358_set_reset_charger(chip, reset);
        break;

	case OPCHG_SMB1357_ID:
		//smb1357_set_reset_charger(chip, reset);
		break;

	case OPCHG_BQ24196_ID:
		bq24196_set_reset_charger(chip, reset);
		break;

	case OPCHG_BQ24157_ID:
		bq24157_set_reset_charger(chip, reset);
		break;

	case OPCHG_BQ24188_ID:
		bq24188_set_reset_charger(chip, reset);
		break;

    default:
        break;
    }
}

void opchg_set_precharger_voltage(struct opchg_charger *chip)
{
	int rc = 0;

	switch (chip->driver_id) {
	case OPCHG_SMB358_ID:
		rc = smb358_set_precharger_voltage(chip);
		break;

	case OPCHG_SMB1357_ID:
		//smb1357_set_reset_charger(chip, reset);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_set_precharger_voltage(chip);
		break;

	default:
		break;
	}
}


void opchg_set_recharger_voltage(struct opchg_charger *chip)
{
	int rc = 0;

    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        //rc = smb358_set_recharger_voltage(chip);
        break;

	case OPCHG_SMB1357_ID:
		//smb1357_set_reset_charger(chip, reset);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_set_recharger_voltage(chip);
		break;

    default:
        break;
    }
}
void opchg_set_prechg_current(struct opchg_charger *chip, int ipre_mA)
{
	int rc = 0;

	switch (chip->driver_id) {
	case OPCHG_SMB358_ID:
		rc = smb358_set_prechg_current(chip,ipre_mA);
		break;

	case OPCHG_SMB1357_ID:
		//smb1357_set_reset_charger(chip, reset);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_set_prechg_current(chip,ipre_mA);
		break;

	default:
		break;
	}
}


void opchg_set_input_chg_current(struct opchg_charger *chip, int current_ma, bool aicl)
{

    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        smb358_set_input_chg_current(chip, current_ma, aicl);
        break;
	case OPCHG_SMB1357_ID:
		smb1357_set_input_chg_current(chip, current_ma, aicl);
		break;

	case OPCHG_BQ24196_ID:
		bq24196_set_input_chg_current(chip, current_ma, aicl);
		break;

	case OPCHG_BQ24157_ID:
		bq24157_set_input_chg_current(chip, current_ma, aicl);
		break;

	case OPCHG_BQ24188_ID:
		bq24188_set_input_chg_current(chip, current_ma, aicl);
		break;

    default:
        break;
    }
}

void opchg_set_fast_chg_current(struct opchg_charger *chip, int mA)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        smb358_set_fastchg_current(chip, mA);
        break;

	case OPCHG_SMB1357_ID:
		smb1357_set_fastchg_current(chip, mA);
		break;

	case OPCHG_BQ24196_ID:
		bq24196_set_fastchg_current(chip, mA);
		break;

	case OPCHG_BQ24157_ID:
		bq24157_set_fastchg_current(chip, mA);
		break;

	case OPCHG_BQ24188_ID:
		bq24188_set_fastchg_current(chip, mA);
		break;

    default:
        break;
    }
}

void opchg_set_float_voltage(struct opchg_charger *chip, int mV)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        smb358_set_float_voltage(chip, mV);
        break;

	case OPCHG_SMB1357_ID:
		smb1357_set_float_voltage(chip, mV);
		break;

	case OPCHG_BQ24196_ID:
		bq24196_set_float_voltage(chip, mV);
		break;

	case OPCHG_BQ24157_ID:
		bq24157_set_float_voltage(chip, mV);
		break;

	case OPCHG_BQ24188_ID:
		bq24188_set_float_voltage(chip, mV);
		break;

    default:
        break;
    }
}

void opchg_set_vindpm_vol(struct opchg_charger *chip, int mV)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
	//
        break;

	case OPCHG_SMB1357_ID:
		//
		break;

	case OPCHG_BQ24196_ID:
		bq24196_set_vindpm_vol(chip, mV);
		break;

	case OPCHG_BQ24157_ID:
		bq24157_set_vindpm_vol(chip, mV);
		break;

	case OPCHG_BQ24188_ID:
		bq24188_set_vindpm_vol(chip, mV);
		break;

    default:
        break;
    }
}

void opchg_set_charging_disable(struct opchg_charger *chip, bool disable)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        smb358_set_charging_disable(chip, disable);
        break;

	case OPCHG_SMB1357_ID:
		smb1357_set_charging_disable(chip, disable);
		/* Cinnamon: Removed debug spam */
		break;

	case OPCHG_BQ24196_ID:
		bq24196_set_charging_disable(chip, disable);
		break;

	case OPCHG_BQ24157_ID:
		bq24157_set_charging_disable(chip, disable);
		break;

	case OPCHG_BQ24188_ID:
		bq24188_set_charging_disable(chip, disable);
		break;

    default:
        break;
    }
}

void opchg_set_suspend_enable(struct opchg_charger *chip, bool enable)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        smb358_set_suspend_enable(chip, enable);
        break;

	case OPCHG_SMB1357_ID:
		smb1357_set_suspend_enable(chip, enable);
		break;

    case OPCHG_BQ24196_ID:
        bq24196_set_suspend_enable(chip, enable);
        break;

	case OPCHG_BQ24157_ID:
        bq24157_set_suspend_enable(chip, enable);
        break;

	case OPCHG_BQ24188_ID:
        bq24188_set_suspend_enable(chip, enable);
        break;

    default:
        break;
    }
}

int opchg_hw_init(struct opchg_charger *chip)
{
    int rc = 0;

    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = smb358_hw_init(chip);
        break;

	case OPCHG_SMB1357_ID:
		rc = smb1357_hw_init(chip);
		break;

	case OPCHG_BQ24196_ID:
		rc = bq24196_hw_init(chip);
		break;

	case OPCHG_BQ24157_ID:
		rc = bq24157_hw_init(chip);
		break;

	case OPCHG_BQ24188_ID:
		rc = bq24188_hw_init(chip);
		break;

    default:
        break;
    }

    return rc;
}

int opchg_get_initial_state(struct opchg_charger *chip)
{
    int rc = 0;

    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        rc = smb358_get_initial_state(chip);
        break;

	case OPCHG_SMB1357_ID:
		rc = smb1357_get_initial_state(chip);
		break;

	case OPCHG_BQ24196_ID:
		if (((is_project(OPPO_15109) && (get_PCB_Version() == HW_VERSION__10||get_PCB_Version() == HW_VERSION__12||get_PCB_Version() == HW_VERSION__14||get_PCB_Version() == HW_VERSION__15)))||is_project(OPPO_15399))
		{
			rc = bq24196_get_initial_state(chip);
		}
		break;

	case OPCHG_BQ24188_ID:
		rc = bq24188_get_initial_state(chip);
		break;

	case OPCHG_BQ24157_ID:
		break;

    default:
        break;
    }

    return rc;
}


void opchg_set_wdt_reset(struct opchg_charger *chip)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        //do nothing
        break;

    case OPCHG_SMB1357_ID:
        //do nothing
        break;

    case OPCHG_BQ24196_ID:
        bq24196_set_wdt_reset(chip);
        break;

	case OPCHG_BQ24188_ID:
        bq24188_set_wdt_reset(chip);
        break;

    default:
        break;
    }
}

void opchg_set_wdt_timer(struct opchg_charger *chip, bool enable)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        //do nothing
        break;

    case OPCHG_SMB1357_ID:
        //do nothing
        break;

    case OPCHG_BQ24196_ID:
		bq24196_set_wdt_timer(chip,enable);
        break;

	case OPCHG_BQ24188_ID:
		bq24188_set_wdt_timer(chip,enable);
        break;

    default:
        break;
    }
}
int opchg_check_charging_pre_full(struct opchg_charger *chip)
{
    int rc = 0;

    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        //do nothing
        break;

    case OPCHG_SMB1357_ID:
        //do nothing
        break;

    case OPCHG_BQ24196_ID:
        bq24196_check_charging_pre_full(chip);
        break;

	case OPCHG_BQ24157_ID:
        bq24157_check_charging_pre_full(chip);
        break;

	case OPCHG_BQ24188_ID:
        bq24188_check_charging_pre_full(chip);
        break;

    default:
        break;
    }

    return rc;
}

int opchg_check_battovp(struct opchg_charger *chip)
{
    int rc = 0;

    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        //do nothing
        break;

    case OPCHG_SMB1357_ID:
        //do nothing
        break;

    case OPCHG_BQ24196_ID:
        rc = bq24196_check_battovp(chip);
        break;

	case OPCHG_BQ24157_ID:
        rc = bq24157_check_battovp(chip);
        break;

	case OPCHG_BQ24188_ID:
        rc = bq24188_check_battovp(chip);
        break;

    default:
        break;
    }

    return rc;
}

int opchg_check_chargerovp(struct opchg_charger *chip)
{
    int rc = 0;

    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        //do nothing
        break;

    case OPCHG_SMB1357_ID:
        //do nothing
        break;

    case OPCHG_BQ24196_ID:
        //rc = bq24196_check_chargerovp(chip);
        break;

	case OPCHG_BQ24157_ID:
        //rc = bq24157_check_chargerovp(chip);
        break;

	case OPCHG_BQ24188_ID:
        rc = bq24188_check_chargerovp(chip);
        break;

    default:
        break;
    }

    return rc;
}


irqreturn_t opchg_chg_irq_handler(int irq, void *dev_id)
{
    struct opchg_charger *chip = dev_id;

    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        smb358_chg_irq_handler(irq, chip);
        break;

	case OPCHG_SMB1357_ID:
		smb1357_chg_irq_handler(irq, chip);
		break;

	case OPCHG_BQ24196_ID:
		break;

    default:
        break;
    }

    return IRQ_HANDLED;
}

void opchg_usbin_valid_irq_handler(bool usb_present)
{
	is_gt1x_tp_charger =usb_present;
	if(!opchg_chip){
		pr_debug("%s opchg_chip is NULL,return\n",__func__);
		return;
	}
	else
	{
		/* Cinnamon: Reduced debug spam - only log when really needed */
		if (opchg_chip->driver_id != OPCHG_BQ24196_ID) {
			pr_debug("%s opchg_chip->driver_id=%d,opchg_chip->chg_present=%d,usb_present=%d\n",__func__,opchg_chip->driver_id,opchg_chip->chg_present,usb_present);
		}
	}

	if(opchg_get_prop_fast_chg_started(opchg_chip) == true){
		/* Cinnamon: Reduced fast charging spam */
		pr_debug("%s fast chg started,return\n",__func__);
		return ;
	}

#if 0
	opchg_chip->chg_present_real = usb_present;
	if(!usb_present && opchg_chip->aicl_working){
		pr_err("%s aicl_working,skip usb plug out\n",__func__);
		return;
	}
#endif

	//add for mmi test,when charger is out status,set factory mode test charging
	if((opchg_chip->chg_present == false) && (usb_present == 1))
	{
		//add for factory mode test charging for 20150325
		opchg_chip->is_factory_mode	= 1;
	}

    switch (opchg_chip->driver_id) {
    case OPCHG_SMB358_ID:
        break;

	case OPCHG_SMB1357_ID:
		break;

	case OPCHG_BQ24196_ID:
		if(usb_present == 1)
		{
			 // when  opchg_chip->chg_present is not Initialization and charger is out status,then Response charger_in Interrupt
			if(opchg_chip->chg_present != true)
			{
				opchg_chip->chg_present = usb_present;
				bq24196_usbin_valid_irq_handler(opchg_chip);
			}
		}
		else
		{
			// if in fast_charging ,and not response charger out
			if(is_project(OPPO_14005) || is_project(OPPO_15011) || is_project(OPPO_15018) || is_project(OPPO_15022)) {
				if(opchg_get_prop_fast_chg_started(opchg_chip) == false){
					opchg_chip->chg_present = usb_present;
					bq24196_usbin_valid_irq_handler(opchg_chip);
				}
			} else {
				opchg_chip->chg_present = usb_present;
				bq24196_usbin_valid_irq_handler(opchg_chip);
			}
		}
		break;

	case OPCHG_BQ24157_ID:
		opchg_chip->chg_present = usb_present;
		bq24157_usbin_valid_irq_handler(opchg_chip);
		break;

	case OPCHG_BQ24188_ID:
		opchg_chip->chg_present = usb_present;
		bq24188_usbin_valid_irq_handler(opchg_chip);
		break;

    default:
        break;
    }

}

void opchg_dump_regs(struct opchg_charger *chip)
{
    switch (chip->driver_id) {
    case OPCHG_SMB358_ID:
        smb358_dump_regs(chip);
        break;

	case OPCHG_SMB1357_ID:
		smb1357_dump_regs(chip);
		break;

	case OPCHG_BQ24196_ID:
		bq24196_dump_regs(chip);
		break;

	case OPCHG_BQ24157_ID:
		bq24157_dump_regs(chip);
		break;

	case OPCHG_BQ24188_ID:
		bq24188_dump_regs(chip);
		break;

    default:
        break;
    }
}

void opchg_switch_to_usbin(struct opchg_charger *chip,bool enable)
{
	int rc=0;

	if(chip != NULL)
	{
		if(chip->usbin_switch_gpio > 0)
		{
			rc=gpio_direction_output(chip->usbin_switch_gpio,enable);
			pr_debug("opchg usbin-switch-gpio:%d\n",gpio_get_value(chip->usbin_switch_gpio));
		}
	}
}

int qpnp_charger_type_get(struct opchg_charger *chip)
{
	union power_supply_propval ret = {0,};

	chip->usb_psy->get_property(chip->usb_psy,
				  POWER_SUPPLY_PROP_TYPE, &ret);

	/* Thermal safety for USB Fast Charge - only if protection enabled */
	if (thermal_protection_enabled && opchg_chip) {
    if (opchg_chip->temperature > USB_FAST_CHARGE_DANGEROUS_TEMP) {
        /* High temperature detected - disable USB Fast Charge */
        if (thermal_disabled == 0) {
            thermal_disabled = 1;
            dev_warn(chip->dev, "USB Fast Charge: Disabled due to high temperature %d°C\n",
                 opchg_chip->temperature / 10);
        }
    } else if (opchg_chip->temperature < USB_FAST_CHARGE_SAFE_TEMP) {
        /* Temperature back to safe level - allow re-enable */
        if (thermal_disabled == 1) {
            thermal_disabled = 0;
            dev_info(chip->dev, "USB Fast Charge: Re-enabled (temperature %d°C)\n",
                 opchg_chip->temperature / 10);
        }
    }
}	else {
		/* Thermal protection disabled */
		thermal_disabled = 0;
	}

	/* Force USB Fast Charge if enabled via sysfs and thermal safe */
	if (force_fast_charge && !thermal_disabled &&
	    (ret.intval == POWER_SUPPLY_TYPE_USB ||
	     ret.intval == POWER_SUPPLY_TYPE_USB_CDP)) {
		ret.intval = POWER_SUPPLY_TYPE_USB_DCP;
	}

	return ret.intval;
}
