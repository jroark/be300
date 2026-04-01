/* dumps the file listing of a cbea file */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tchar.h>


int main(int argc, char *argv[])
{
	if(argc == 2)
	{
		FILE	*fpCbea	= NULL;
		int		b		= 0,
				fc		= 0,
				tmp		= 0,
				size	= 0,
				offset	= 0;

		char	szFileName[256];
		wchar_t	wszCasioHeader[0x25];

		fpCbea = fopen(argv[1], "rb");

		if(fpCbea == NULL)
		{
			fprintf(stderr, "Error: opening %s for reading...\n", argv[1]);
			return -1;
		}


		/* seek past first two bytes to the file count... */
		fseek(fpCbea, 2, SEEK_SET);
		fread((void *)&fc, sizeof(int), 1, fpCbea);
		printf("%d files in %s\n", fc, argv[1]);

		fseek(fpCbea, 2, SEEK_CUR);
		memset((void *)wszCasioHeader, 0, 0x25 * sizeof(wchar_t));
		fread((void *)wszCasioHeader, 0x24, sizeof(wchar_t), fpCbea);

		printf("%S\n\n", wszCasioHeader);

		/* seek past the header... who cares */
		fseek(fpCbea, 256, SEEK_SET);

		while(b < fc)
		{
			/* read offset of next file */
			fread((void *)&offset, sizeof(int), 1, fpCbea);
			printf("offset of next file %d\n", offset);

			/* seek to length of filename */
			fseek(fpCbea, 3, SEEK_CUR);

			/* length of filename */
			fread((void *)&tmp, 1, 1, fpCbea);

			memset(szFileName, 0, 256);
			/* read the filename */
			fread((void *)szFileName, 1, tmp+1, fpCbea);
			printf("%s", szFileName);

			/* filesize */
			fread((void *)&size, sizeof(int), 1, fpCbea);
			printf(" is %d bytes long\n\n", size);

			fseek(fpCbea, size, SEEK_CUR);

			b++;
		}

	}

	return 0;
}
