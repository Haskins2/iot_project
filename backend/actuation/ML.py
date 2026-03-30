import pandas as pd
import numpy as np
from sklearn.model_selection import train_test_split
from sklearn.metrics import mean_absolute_error, mean_squared_error, r2_score
import xgboost as xgb

import requests
import xml.etree.ElementTree as ET

"""
~~~FOR USAGE~~~
in your code create the model
m = model("train_data.csv", target_column="flood_chance")
m.train()

-- Data must contain these columns in this order --
data = pd.DataFrame({
    "rainfall": [14.2],
    "temperature": [7.5],
    "river_level": [4.0],
    "humidity": [83]
})

prediction = m.predict(data)
print(prediction)

-- prediction is a float between 0 and 1 --

"""





class model:
    def __init__(self, train_data_path, target_column=None):
        self.train_data_path = train_data_path
        self.target_column = target_column
        self.model = None
        self.feature_columns = None

    def train(self):
        df = pd.read_csv(self.train_data_path)

        if df.empty:
            raise ValueError("Training dataframe is empty.")

        if self.target_column is None:
            self.target_column = df.columns[-1]

        if self.target_column not in df.columns:
            raise ValueError(f"Target column '{self.target_column}' not found in dataframe.")

        X = df.drop(columns=[self.target_column])
        y = df[self.target_column]

        X = pd.get_dummies(X, drop_first=True)
        self.feature_columns = X.columns.tolist()

        X_train, X_test, y_train, y_test = train_test_split(
            X,
            y,
            test_size=0.2,
            random_state=42
        )

        self.model = xgb.XGBRegressor(
            n_estimators=100,
            max_depth=4,
            learning_rate=0.1,
            objective="reg:squarederror",
            eval_metric="rmse",
            random_state=42
        )

        self.model.fit(X_train, y_train)

        y_pred = self.model.predict(X_test)

        print("Training complete.")
        print("MAE :", mean_absolute_error(y_test, y_pred))
        print("RMSE:", mean_squared_error(y_test, y_pred) ** 0.5)
        print("R2  :", r2_score(y_test, y_pred))

    def predict(self, data):
        if self.model is None:
            raise ValueError("Call train() before predict().")

        if isinstance(data, str):
            X_new = pd.read_csv(data)
        else:
            X_new = data.copy()

        if self.target_column in X_new.columns:
            X_new = X_new.drop(columns=[self.target_column])

        X_new = pd.get_dummies(X_new, drop_first=True)
        X_new = X_new.reindex(columns=self.feature_columns, fill_value=0)

        preds = self.model.predict(X_new)
        preds = np.clip(preds, 0, 1)

        return pd.DataFrame({
            "predicted_flood_chance": preds
        })