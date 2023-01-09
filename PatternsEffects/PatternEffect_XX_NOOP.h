// This effect does nothing, intentionally. Good for when you sometimes want... nothing.

#ifndef PatternEffectNOOP_H
#define PatternEffectNOOP_H

class PatternEffectNOOP : public Drawable {
    
private:


public:

    PatternEffectNOOP() {

      name = (char *)"Doing Nothing";
      id = "!";
      enabled = true;

    }


    void start(uint8_t _pattern) {
        
    }

    unsigned int drawFrame(uint8_t _pattern, uint8_t _total) {

        return 0;

    }

};

#endif