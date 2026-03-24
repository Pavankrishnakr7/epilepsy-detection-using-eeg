from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import Dense, LSTM

def build_model():
    model = Sequential()
    model.add(LSTM(64, input_shape=(10,1)))
    model.add(Dense(32, activation='relu'))
    model.add(Dense(1, activation='sigmoid'))

    model.compile(optimizer='adam', loss='binary_crossentropy', metrics=['accuracy'])
    return model
