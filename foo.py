import random
import time

def gamble():
    roll = random.randint(1, 100)
    if roll <= 5:
        print("🎉 JACKPOT! You win 100x your bet!")
    elif roll <= 15:
        print("✨ Big Win! You win 10x your bet!")
    elif roll <= 35:
        print("👍 Small Win! You win 2x your bet!")
    else:
        print("😞 You lose. Try again!")

if __name__ == "__main__":
    while True:
        gamble()
        time.sleep(1)
