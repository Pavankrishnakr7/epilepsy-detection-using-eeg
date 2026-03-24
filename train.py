import numpy as np
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import Dense, LSTM

X = np.random.rand(100, 10, 1)
y = np.random.randint(0, 2, 100)

model = Sequential()
model.add(LSTM(64, input_shape=(10,1)))
model.add(Dense(1, activation='sigmoid'))

model.compile(optimizer='adam', loss='binary_crossentropy', metrics=['accuracy'])

model.fit(X, y, epochs=5)

loss, accuracy = model.evaluate(X, y)
print("Accuracy:", accuracy)
