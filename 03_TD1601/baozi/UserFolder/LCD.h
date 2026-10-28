#ifndef __LCD_H
#define __LCD_H		
// #include "sys.h"	 

#include <stdint.h>
#include <stddef.h>
	 
////////////////////////////////////////////////////////////////////////	 
//
#define USE_LCM_DIR  	  0  	// 	0-01-1802-2703-90

	 
//LCD
typedef struct  
{										    
	uint16_t width;			//LCD 
	uint16_t height;			//LCD 
	uint16_t id;				//LCD ID
	uint8_t  dir;			//01	
	uint16_t	wramcmd;		//gram
	uint16_t setxcmd;		//x
	uint16_t  setycmd;		//y	 
}_lcd_dev; 	  

//LCD
extern _lcd_dev lcddev;	//LCD
//LCD	   
extern uint16_t  POINT_COLOR;//    
extern uint16_t  BACK_COLOR; //.

////////////////////////////////////////////////////////////////////
//-----------------LCD----------------// 

extern int ioctl(uint32_t value);
extern int dc_ioctl(uint32_t value);
extern int led_ioctl(uint32_t value);
extern int rst_ioctl(uint32_t value);

#define	LCD_RST_SET  rst_ioctl(1)    //  	    	 PC15
#define	LCD_CS_SET  ioctl(1)    //  	     PA12
#define	LCD_RS_SET	dc_ioctl(1)    ///       PA15	   
#define	LCD_WR_SET	    //					 PC13
#define	LCD_RD_SET	    //					 PC14

#define	LCD_RST_CLR  rst_ioctl(0)    //  	    		 PC15
#define	LCD_CS_CLR  ioctl(0)     //  	     PA12
#define	LCD_RS_CLR	dc_ioctl(0)     ///       PA15   
#define	LCD_WR_CLR	     //					 PC13
#define	LCD_RD_CLR	     //					 PC14  	

#define	LCD_LED led_ioctl(1) // 

//PB0~15,
#define DATAOUT(x) (*(volatile uint32_t *)0x6000C060 = x) //
#define DATAIN  0      //	
	 

//
#define WHITE         	 0xFFFF
#define BLACK         	 0x0000	  
#define BLUE         	   0x001F  
#define BRED             0XF81F
#define GRED 			       0XFFE0
#define GBLUE			       0X07FF
#define RED           	 0xF800
#define MAGENTA       	 0xF81F
#define GREEN         	 0x07E0
#define CYAN          	 0x7FFF
#define YELLOW        	 0xFFE0
#define BROWN 			 0XBC40 //
#define BRRED 			 0XFC07 //
#define GRAY  			 0X8430 //
//GUI

#define DARKBLUE      	 0X01CF	//
#define LIGHTBLUE      	 0X7D7C	//  
#define GRAYBLUE       	 0X5458 //
//PANEL 
 
#define LIGHTGREEN     	 0X841F // 
#define LGRAY 					 0XC618 //(PANNEL),

#define LGRAYBLUE        0XA651 //()
#define LBBLUE           0X2B12 //()

typedef void (*LCD_WR_REG_t)(uint16_t data);
extern LCD_WR_REG_t	LCD_WR_REG; 

typedef void (*LCD_WR_DATA_t)(uint16_t data);
extern LCD_WR_DATA_t	LCD_WR_DATA;

typedef void (*LCD_WR_DATA2_t)(uint16_t data);
extern LCD_WR_DATA2_t	LCD_WR_DATA2;	    															  
	    															  
void LCD_Init(void);													   	//
void LCD_DisplayOn(void);													//
void LCD_DisplayOff(void);													//
void LCD_Clear(uint16_t Color);	 												//
void LCD_SetCursor(uint16_t Xpos, uint16_t Ypos);										//
void LCD_DrawPoint(uint16_t x,uint16_t y);											//
void LCD_Fast_DrawPoint(uint16_t x,uint16_t y,uint16_t color);								//
void LCD_Draw_Circle(uint16_t x0,uint16_t y0,uint8_t r);						 			//
void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);							//
void LCD_DrawRectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);		   				//
void LCD_Fill(uint16_t sx,uint16_t sy,uint16_t ex,uint16_t ey,uint16_t color);		   				//
void LCD_Color_Fill(uint16_t sx,uint16_t sy,uint16_t ex,uint16_t ey,uint16_t color);				//
void LCD_ShowChar(uint16_t x,uint16_t y,uint8_t num,uint8_t size,uint8_t mode);						//
void LCD_ShowNum(uint16_t x,uint16_t y,uint32_t num,uint8_t len,uint8_t size);  						//
void LCD_ShowxNum(uint16_t x,uint16_t y,uint32_t num,uint8_t len,uint8_t size,uint8_t mode);				// 
void LCD_ShowString(uint16_t x,uint16_t y,uint16_t width,uint16_t height,uint8_t size,uint8_t *p);		//,12/16

void LCD_WriteReg(uint16_t LCD_Reg, uint16_t LCD_RegValue);
void LCD_WriteRAM_Prepare(void);
void LCD_WriteRAM(uint16_t RGB_Code);
void Load_Drow_Dialog(void);
void LCD_Set_Window(uint16_t sx,uint16_t sy,uint16_t width,uint16_t height);	//				
void Show_Str(uint16_t x, uint16_t y,uint8_t *str,uint8_t size,uint8_t mode);//
void Gui_Drawbmp16(uint16_t x,uint16_t y,const unsigned char *p); //40*40 
void Gui_StrCenter(uint16_t x, uint16_t y,uint8_t *str,uint8_t size,uint8_t mode);//
void LCD_Display_Dir(uint8_t dir); //LCD
void lcd_draw_bline(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,uint8_t size,uint16_t color);//
void gui_fill_circle(uint16_t x0,uint16_t y0,uint16_t r,uint16_t color);//
void gui_draw_hline(uint16_t x0,uint16_t y0,uint16_t len,uint16_t color);// 
uint16_t LCD_ReadPoint(uint16_t x,uint16_t y);//
uint16_t LCD_Read_ID(uint8_t reg);//ID


#endif













