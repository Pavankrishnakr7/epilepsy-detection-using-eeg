import numpy as np

def preprocess_data():
    # Dummy EEG preprocessing
    data = np.random.rand(100, 10)
    
    # Normalize data
    data = (data - np.mean(data)) / np.std(data)
    
    return data

if __name__ == "__main__":
    processed = preprocess_data()
    print("Data processed:", processed.shape)
