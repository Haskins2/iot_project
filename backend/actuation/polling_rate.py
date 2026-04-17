from datetime import datetime

import pandas as pd
import numpy as np

import time
import joblib

import requests
import xml.etree.ElementTree as ET

from ML import model

USE_PRETRAINED_MODEL = True
def weather_xml_to_dataframe(xml_text: str) -> pd.DataFrame:
    root = ET.fromstring(xml_text)

    rows = {}

    for time_elem in root.findall(".//time"):
        from_ts = time_elem.attrib.get("from")
        to_ts = time_elem.attrib.get("to")

        row_key = to_ts if to_ts else from_ts

        if row_key not in rows:
            rows[row_key] = {
                "datetime": row_key,
                "from": from_ts,
                "to": to_ts,
            }

        location = time_elem.find("location")
        if location is None:
            continue

        row = rows[row_key]

        row["altitude"] = location.attrib.get("altitude")
        row["latitude"] = location.attrib.get("latitude")
        row["longitude"] = location.attrib.get("longitude")

        for child in location:
            tag = child.tag

            if tag == "temperature":
                row["temperature_c"] = child.attrib.get("value")
            elif tag == "windDirection":
                row["wind_direction_deg"] = child.attrib.get("deg")
                row["wind_direction_name"] = child.attrib.get("name")
            elif tag == "windSpeed":
                row["wind_speed_mps"] = child.attrib.get("mps")
                row["wind_speed_beaufort"] = child.attrib.get("beaufort")
                row["wind_speed_name"] = child.attrib.get("name")
            elif tag == "windGust":
                row["wind_gust_mps"] = child.attrib.get("mps")
            elif tag == "globalRadiation":
                row["global_radiation"] = child.attrib.get("value")
            elif tag == "humidity":
                row["humidity_percent"] = child.attrib.get("value")
            elif tag == "pressure":
                row["pressure_hpa"] = child.attrib.get("value")
            elif tag == "cloudiness":
                row["cloudiness_percent"] = child.attrib.get("percent")
            elif tag == "lowClouds":
                row["low_clouds_percent"] = child.attrib.get("percent")
            elif tag == "mediumClouds":
                row["medium_clouds_percent"] = child.attrib.get("percent")
            elif tag == "highClouds":
                row["high_clouds_percent"] = child.attrib.get("percent")
            elif tag == "dewpointTemperature":
                row["dewpoint_c"] = child.attrib.get("value")
            elif tag == "precipitation":
                row["precipitation_mm"] = child.attrib.get("value")
                row["precipitation_min_mm"] = child.attrib.get("minvalue")
                row["precipitation_max_mm"] = child.attrib.get("maxvalue")
                row["precipitation_probability"] = child.attrib.get("probability")
            elif tag == "symbol":
                row["symbol_id"] = child.attrib.get("id")
                row["symbol_number"] = child.attrib.get("number")
            else:
                for attr_name, attr_val in child.attrib.items():
                    row[f"{tag}_{attr_name}"] = attr_val

    df = pd.DataFrame(rows.values()).sort_values("datetime").reset_index(drop=True)
    df["datetime"] = pd.to_datetime(df["datetime"], utc=True)

    for col in df.columns:
        if col not in ["datetime", "from", "to", "wind_direction_name", "wind_speed_name", "symbol_id"]:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    return df

def get_current_date_time() :
    date_time = datetime.now()
    date = str(date_time).split(" ")[0]
    time = str(date_time).split(" ")[1].split(".")[0]
    return date, time

def get_datetime_filter_string(hours_out=1) :
    #gets the string to filter datetime, currentime + hourshout
    date, time = get_current_date_time()
    time_to_filter = int(time.split(':')[0]) + hours_out
    if time_to_filter > 23 :
        time_to_filter = time_to_filter - 24
    return f"{date} {time_to_filter}:00:00+00:00"

def get_sensor_water_level() :
    return 0

def get_sensor_rain() :
    return 0


def get_polling_rate() :
    url = "http://openaccess.pf.api.met.ie/metno-wdb2ts/locationforecast?lat=54.7210798611;long=-8.7237392806"

    resp = requests.get(url, timeout=10)
    resp.raise_for_status()

    root = ET.fromstring(resp.text)

    with open("met_forecast.xml", "w", encoding="utf-8") as f:
        f.write(resp.text)

    df = weather_xml_to_dataframe(resp.text)

    new_df = df[df["datetime"] == get_datetime_filter_string()]

    precipitation_probability = new_df["precipitation_probability"].iloc[0]
    temperature_c = new_df["temperature_c"].iloc[0]
    wind_speed_mps = new_df["wind_speed_mps"].iloc[0]
    humidity_percent = new_df["humidity_percent"].iloc[0]
    cloudiness_percent = new_df["cloudiness_percent"].iloc[0]
    pressure_hpa = new_df["pressure_hpa"].iloc[0]
    precipitation_mm = new_df["precipitation_mm"].iloc[0]

    prediction_input = pd.DataFrame([{
        "precipitation_probability": new_df["precipitation_probability"].iloc[0],
        "temperature_c": new_df["temperature_c"].iloc[0],
        "wind_speed_mps": new_df["wind_speed_mps"].iloc[0],
        "humidity_percent": new_df["humidity_percent"].iloc[0],
        "cloudiness_percent": new_df["cloudiness_percent"].iloc[0],
        "pressure_hpa": new_df["pressure_hpa"].iloc[0],
        "precipitation_mm": new_df["precipitation_mm"].iloc[0]
    }])

    sensor_waterlevel = get_sensor_water_level()
    sensor_rain = get_sensor_rain()

    if USE_PRETRAINED_MODEL :
        m = joblib.load("model/flood_model.joblib")
    else :
        m = model("data/synthetic_weather_with_flood_labels.csv", target_column="flood_label")
        m.train()
        joblib.dump(m, "flood_model.joblib")
    flood_prob = m.predict(prediction_input)
    flood_prob_round = round(flood_prob)
    return flood_prob_round



if __name__ == "__main__":
    get_polling_rate()