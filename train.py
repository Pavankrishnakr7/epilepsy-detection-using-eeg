import numpy as np
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import Dense, LSTM

# Simulated EEG data
X = np.random.rand(200, 10, 1)
y = np.random.randint(0, 2, 200)

# Model
model = Sequential()
model.add(LSTM(64, return_sequences=False, input_shape=(10,1)))
model.add(Dense(32, activation='relu'))
model.add(Dense(1, activation='sigmoid'))

# Compile
model.compile(optimizer='adam', loss='binary_crossentropy', metrics=['accuracy'])

# Train
history = model.fit(X, y, epochs=10, validation_split=0.2)

# Evaluate
loss, accuracy = model.evaluate(X, y)
print("Final Accuracy:", accuracy)
